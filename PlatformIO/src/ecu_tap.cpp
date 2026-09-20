#include "ecu_tap.h"

#include <Arduino.h>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "defs.h"
#include "ecu_decoder.h"

namespace ecu {

static Decoder s_decoder;

// --- rolling stats -------------------------------------------------------
static volatile uint32_t s_in_frames = 0, s_in_bytes = 0;
static volatile uint32_t s_out_frames = 0, s_out_bytes = 0;
static volatile uint32_t s_dropped = 0;
// Who owns the record ring. Exactly one consumer at a time — the TCP tapcap
// client or the in-browser capture — otherwise they'd interleave and both
// files would be garbage.
enum Owner : uint8_t { OWNER_NONE = 0, OWNER_TCP, OWNER_WS };
static volatile Owner s_owner = OWNER_NONE;
static bool s_serial_dump = false;

// --- byte ring buffer for the capture stream -----------------------------
// 16 KB ≈ 0.5 s of a live 704 B/25 ms stream — enough to ride out a WiFi
// hiccup on the browser path without dropping records.
static constexpr size_t kRingCap = 16384;
static uint8_t s_ring[kRingCap];
static volatile size_t s_head = 0;  // write index
static volatile size_t s_tail = 0;  // read index
static SemaphoreHandle_t s_ring_mtx = nullptr;

static size_t ring_free_() {
  size_t used = (s_head + kRingCap - s_tail) % kRingCap;
  return kRingCap - 1 - used;
}

// Push a framed record. Drops the whole record if it doesn't fit (never blocks).
static void ring_push_(uint8_t dir, uint8_t ep, const uint8_t *data, size_t len) {
  if (s_ring_mtx == nullptr || len > 0xFFFF) return;
  if (s_owner == OWNER_NONE) return;  // nobody capturing → don't fill the ring or count drops
  uint8_t hdr[8];
  hdr[0] = 0xEC;
  hdr[1] = dir;
  hdr[2] = ep;
  hdr[3] = 0;
  uint32_t t = millis();
  memcpy(&hdr[4], &t, 4);
  uint16_t l = (uint16_t)len;
  xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
  if (ring_free_() < sizeof(hdr) + 2 + len) {
    s_dropped++;
    xSemaphoreGive(s_ring_mtx);
    return;
  }
  auto put = [](const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      s_ring[s_head] = p[i];
      s_head = (s_head + 1) % kRingCap;
    }
  };
  put(hdr, sizeof(hdr));
  put(reinterpret_cast<uint8_t *>(&l), 2);
  put(data, len);
  xSemaphoreGive(s_ring_mtx);
}

// Drain up to out_cap bytes into out. Returns bytes copied.
static size_t ring_drain_(uint8_t *out, size_t out_cap) {
  if (s_ring_mtx == nullptr) return 0;
  size_t copied = 0;
  xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
  while (s_tail != s_head && copied < out_cap) {
    out[copied++] = s_ring[s_tail];
    s_tail = (s_tail + 1) % kRingCap;
  }
  xSemaphoreGive(s_ring_mtx);
  return copied;
}

static void serial_dump_(const char *tag, uint8_t ep, const uint8_t *data, size_t len) {
  if (!s_serial_dump) return;
  char hex[3 * 24 + 4];
  int n = 0;
  for (size_t i = 0; i < len && i < 24 && n < (int)sizeof(hex) - 4; i++) {
    n += snprintf(hex + n, sizeof(hex) - n, "%02X ", data[i]);
  }
  DEBUG_TAP("%s ep%u len=%u: %s", tag, ep, (unsigned)len, hex);
}

// --- bridge hooks --------------------------------------------------------
void on_bulk_in(uint8_t ep, const uint8_t *data, size_t len) {
  if (len == 0) return;
  s_in_frames++;
  s_in_bytes += len;
#if ECU_DATA_EP == 0
  s_decoder.feed(data, len);
#else
  if (ep == ECU_DATA_EP) s_decoder.feed(data, len);
#endif
  serial_dump_("IN ", ep, data, len);
  ring_push_(1, ep, data, len);
}

void on_bulk_out(uint8_t ep, const uint8_t *data, size_t len) {
  if (len == 0) return;
  s_out_frames++;
  s_out_bytes += len;
  serial_dump_("OUT", ep, data, len);
  ring_push_(0, ep, data, len);
}

// --- ECU identity ----------------------------------------------------------
// Ignitron's session start reads five 8-byte identity words with bRequest
// 0xE2 (wValue = bank, wIndex = word address). Bank 2/3 replies come back
// XOR-obfuscated with the same keystream the tune uses (transcribed from the
// exe's read routine; tools/ignitron_tune.py is the reference copy). The
// exe's connect routine keeps these bytes and its firmware-manager dialog
// prints them as "<major>.<minor>":
//   bank 1 word 0x005  bytes 0,1  -> COM bootloader      ("1.0")
//   bank 1 word 0x141  bytes 4,5  -> COM firmware        ("1.2")
//   bank 2 word 0x040  bytes 1,0  -> DSP bootloader      ("1.3")
//   bank 2 word 0x140  bytes 1,0  -> DSP firmware        ("1.28")
//   bank 3 word 0x040  bytes 1,0  -> CPU bootloader      ("1.1")
//   bank 3 word 0x140  bytes 1,0  -> CPU firmware        ("1.28")
static void keystream_(uint8_t bank, uint16_t word, uint8_t ks[8]) {
  uint16_t d = (uint16_t)(word + (bank == 2 ? 0x5A68 : 0x4991));
  uint16_t m = (uint16_t)((uint16_t)(d - 2) * d);
  uint16_t n = (uint16_t)(0 - m);
  uint16_t p = (uint16_t)(n + 0x381);
  uint16_t q = (uint16_t)(0 - p);
  ks[0] = m & 0xFF; ks[1] = m >> 8; ks[2] = n & 0xFF; ks[3] = n >> 8;
  ks[4] = p & 0xFF; ks[5] = p >> 8; ks[6] = q & 0xFF; ks[7] = q >> 8;
}

static void note_identity_(const uint8_t *setup8, const uint8_t *data, size_t len) {
  if (setup8[0] != 0xC0 || setup8[1] != 0xE2 || len < 8) return;
  uint16_t bank = setup8[2] | (setup8[3] << 8);
  uint16_t word = setup8[4] | (setup8[5] << 8);
  uint8_t w[8];
  memcpy(w, data, 8);
  if (bank == 2 || bank == 3) {
    uint8_t ks[8];
    keystream_((uint8_t)bank, word, ks);
    for (int i = 0; i < 8; i++) w[i] ^= ks[i];
  }
  if (bank == 1 && word == 0x005) g_telemetry.set_ecu_version(UNIT_COM, false, w[0], w[1]);
  else if (bank == 1 && word == 0x141) g_telemetry.set_ecu_version(UNIT_COM, true, w[4], w[5]);
  else if ((bank == 2 || bank == 3) && (word == 0x040 || word == 0x140))
    g_telemetry.set_ecu_version(bank == 2 ? UNIT_DSP : UNIT_CPU, word == 0x140, w[1], w[0]);
}

void on_control(uint8_t dir, const uint8_t *setup8, const uint8_t *data, size_t len) {
  static constexpr size_t kMaxCtrlData = 256;
  if (dir == 1 && data) note_identity_(setup8, data, len);
  if (len > kMaxCtrlData) len = kMaxCtrlData;   // capture diagnostic only, never blocks the bridge
  uint8_t tmp[8 + kMaxCtrlData];
  memcpy(tmp, setup8, 8);
  if (data && len) memcpy(tmp + 8, data, len);
  serial_dump_(dir ? "CTL-IN " : "CTL-OUT", /*ep=*/0, tmp, 8 + len);
  ring_push_(dir, /*ep=*/0, tmp, 8 + len);
}

void tap_set_serial_dump(bool on) { s_serial_dump = on; }

TapStats tap_stats() {
  return TapStats{s_in_frames, s_in_bytes, s_out_frames, s_out_bytes,
                  s_dropped, s_owner != OWNER_NONE};
}

// --- ownership -----------------------------------------------------------
// Atomically take the ring for `who`. Fails if anyone else holds it.
static bool claim_(Owner who) {
  if (s_ring_mtx == nullptr) return false;
  xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
  bool ok = (s_owner == OWNER_NONE);
  if (ok) {
    s_owner = who;
    s_tail = s_head;      // start clean
    s_dropped = 0;
  }
  xSemaphoreGive(s_ring_mtx);
  return ok;
}

static void release_(Owner who) {
  if (s_owner == who) s_owner = OWNER_NONE;
}

// --- in-browser capture ----------------------------------------------------
bool tap_capture_open() {
  bool ok = claim_(OWNER_WS);
  if (ok) DEBUG_TAP("browser capture started");
  else DEBUG_TAP("browser capture refused: ring owned by %s",
                 s_owner == OWNER_TCP ? "TCP client" : "another capture");
  return ok;
}

void tap_capture_close() {
  if (s_owner == OWNER_WS) DEBUG_TAP("browser capture stopped (dropped=%lu)", (unsigned long)s_dropped);
  release_(OWNER_WS);
}

bool tap_capture_active() { return s_owner == OWNER_WS; }

size_t tap_capture_read(uint8_t *out, size_t cap) {
  if (s_owner != OWNER_WS) return 0;
  return ring_drain_(out, cap);
}

void tap_note(const char *text) {
  if (!text) return;
  size_t len = strlen(text);
  if (len > 200) len = 200;
  // 0xFF is the marker "direction" tapcap.py uses for --note records.
  ring_push_(0xFF, /*ep=*/0, reinterpret_cast<const uint8_t *>(text), len);
}

// --- capture server task -------------------------------------------------
static void tap_server_task(void *arg) {
  uint16_t port = (uint16_t)(uintptr_t)arg;

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    DEBUG_TAP("socket() failed: %d", errno);
    vTaskDelete(nullptr);
    return;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(listen_fd, 1) < 0) {
    DEBUG_TAP("bind/listen failed: %d", errno);
    close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  DEBUG_TAP("capture server listening on TCP:%u", port);

  static uint8_t chunk[1024];
  for (;;) {
    sockaddr_in cli{};
    socklen_t clilen = sizeof(cli);
    int fd = accept(listen_fd, (sockaddr *)&cli, &clilen);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!claim_(OWNER_TCP)) {
      // A browser capture owns the ring — refuse rather than interleave.
      DEBUG_TAP("capture client refused: browser capture in progress");
      close(fd);
      continue;
    }
    DEBUG_TAP("capture client connected");

    for (;;) {
      size_t n = ring_drain_(chunk, sizeof(chunk));
      if (n == 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      ssize_t sent = send(fd, chunk, n, 0);
      if (sent < 0) break;
    }
    close(fd);
    release_(OWNER_TCP);
    DEBUG_TAP("capture client disconnected");
  }
}

void tap_begin(uint16_t tcp_port) {
  if (s_ring_mtx == nullptr) s_ring_mtx = xSemaphoreCreateMutex();
  if (tcp_port != 0) {
    xTaskCreatePinnedToCore(tap_server_task, "ecu_tap", 4096,
                            (void *)(uintptr_t)tcp_port, 3, nullptr, 0);
  }
  DEBUG_ECU("decoder %s", Decoder::configured() ? "active" : "awaiting layout");
}

}  // namespace ecu
