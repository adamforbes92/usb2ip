#include "logger.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "defs.h"
#include "gauges.h"

namespace logger {

// --- configuration ------------------------------------------------------------
static Preferences s_prefs;
static bool s_prefs_open = false;
static uint8_t s_sel[(gauges::CH_COUNT_TOTAL + 7) / 8] = {0};   // channel bitmap
static uint8_t s_rate_hz = 10;

static inline bool sel_get(uint16_t ch) { return (s_sel[ch >> 3] >> (ch & 7)) & 1; }

size_t channels(uint16_t *out, size_t cap) {
  size_t n = 0;
  for (uint16_t ch = 0; ch < gauges::CH_COUNT_TOTAL && n < cap; ch++)
    if (sel_get(ch)) out[n++] = ch;
  return n;
}

static size_t selected_count() {
  size_t n = 0;
  for (uint16_t ch = 0; ch < gauges::CH_COUNT_TOTAL; ch++) n += sel_get(ch);
  return n;
}

bool set_channels(const uint16_t *chs, size_t n) {
  if (n > MAX_CHANNELS) return false;
  uint8_t sel[sizeof(s_sel)] = {0};
  for (size_t i = 0; i < n; i++) {
    if (chs[i] >= gauges::CH_COUNT_TOTAL) return false;
    sel[chs[i] >> 3] |= 1 << (chs[i] & 7);
  }
  memcpy(s_sel, sel, sizeof(s_sel));
  if (s_prefs_open) s_prefs.putBytes("sel", s_sel, sizeof(s_sel));
  return true;
}

uint8_t rate_hz() { return s_rate_hz; }
void set_rate_hz(uint8_t hz) {
  if (hz < 1) hz = 1;
  if (hz > 20) hz = 20;
  s_rate_hz = hz;
  if (s_prefs_open) s_prefs.putUChar("rate", hz);
}

// --- device logging -------------------------------------------------------------
// Ring buffer between the sampler (USB poll context) and the writer task.
// Allocated for the life of a recording only, so an idle logger costs the heap
// nothing. 16 KB = ~1 s at the worst case (all 384 channels at 20 Hz, 15 KB/s);
// the writer drains every 100 ms.
static constexpr size_t kRingCap = 16 * 1024;
static uint8_t *s_ring = nullptr;
static volatile size_t s_head = 0, s_tail = 0;
static SemaphoreHandle_t s_mtx = nullptr;       // ring buffer (held briefly)
static SemaphoreHandle_t s_file_mtx = nullptr;  // file handle (held across flash writes)
static TaskHandle_t s_writer = nullptr;

static volatile bool s_running = false;
static File s_file;
static char s_name[32] = {0};
static uint16_t s_chs[MAX_CHANNELS];
static size_t s_nch = 0;
static uint32_t s_rows = 0, s_bytes = 0, s_dropped = 0, s_started_ms = 0, s_last_sample_ms = 0;

static size_t ring_used() { return (s_head + kRingCap - s_tail) % kRingCap; }

static bool ring_put(const uint8_t *p, size_t n) {
  if (!s_ring || kRingCap - 1 - ring_used() < n) return false;
  for (size_t i = 0; i < n; i++) { s_ring[s_head] = p[i]; s_head = (s_head + 1) % kRingCap; }
  return true;
}

static size_t ring_take(uint8_t *out, size_t cap) {
  size_t n = 0;
  if (!s_ring) return 0;
  while (s_tail != s_head && n < cap) { out[n++] = s_ring[s_tail]; s_tail = (s_tail + 1) % kRingCap; }
  return n;
}

// Writer: drains the ring to the file in chunks, flushes every 2 s.
static void writer_task(void *) {
  uint8_t chunk[1024];
  uint32_t last_flush = millis();
  for (;;) {
    if (!s_running) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    size_t n;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    n = ring_take(chunk, sizeof(chunk));
    xSemaphoreGive(s_mtx);
    if (n) {
      xSemaphoreTake(s_file_mtx, portMAX_DELAY);
      if (s_file) s_file.write(chunk, n);
      xSemaphoreGive(s_file_mtx);
      s_bytes += n;
      continue;                       // keep draining while there is data
    }
    uint32_t now = millis();
    if (now - last_flush >= 2000) {
      xSemaphoreTake(s_file_mtx, portMAX_DELAY);
      if (s_file) s_file.flush();
      xSemaphoreGive(s_file_mtx);
      last_flush = now;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool running() { return s_running; }

bool start(const char *name, char *name_out, size_t cap) {
  if (s_running) return false;
  s_nch = channels(s_chs, MAX_CHANNELS);
  if (s_nch == 0) return false;
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);

  // File name: the user's (sanitised), else a running number. Either way it
  // must not collide with an existing log.
  char base[25] = {0};
  size_t bl = 0;
  for (const char *p = name; p && *p && bl < sizeof(base) - 1; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '_' || c == '-') base[bl++] = c;
    else if (c == ' ') base[bl++] = '_';
  }
  if (bl == 0) {
    uint32_t seq = s_prefs_open ? s_prefs.getUInt("seq", 0) + 1 : (uint32_t)(millis() / 1000);
    if (s_prefs_open) s_prefs.putUInt("seq", seq);
    snprintf(base, sizeof(base), "log_%04lu", (unsigned long)seq);
  }
  snprintf(s_name, sizeof(s_name), "%s.ilg", base);
  for (int n = 2; LittleFS.exists(String(kDir) + "/" + s_name) && n < 100; n++)
    snprintf(s_name, sizeof(s_name), "%s_%d.ilg", base, n);
  String path = String(kDir) + "/" + s_name;
  s_file = LittleFS.open(path, FILE_WRITE);
  if (!s_file) {
    DEBUG_ECU("log: cannot create %s", path.c_str());
    return false;
  }
  // header
  uint8_t hdr[12];
  memcpy(hdr, "IGLG", 4);
  hdr[4] = kVersion; hdr[5] = s_rate_hz;
  hdr[6] = (uint8_t)(s_nch & 0xFF); hdr[7] = (uint8_t)(s_nch >> 8);
  memset(hdr + 8, 0, 4);
  s_file.write(hdr, sizeof(hdr));
  for (size_t i = 0; i < s_nch; i++) {
    uint8_t b[2] = {(uint8_t)(s_chs[i] & 0xFF), (uint8_t)(s_chs[i] >> 8)};
    s_file.write(b, 2);
  }
  s_file.flush();

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (!s_ring) s_ring = (uint8_t *)malloc(kRingCap);
  s_head = s_tail = 0;
  xSemaphoreGive(s_mtx);
  if (!s_ring) { s_file.close(); DEBUG_ECU("log: ring alloc failed"); return false; }
  s_rows = 0; s_bytes = sizeof(hdr) + 2 * s_nch; s_dropped = 0;
  s_started_ms = millis(); s_last_sample_ms = 0;
  s_running = true;
  if (name_out && cap) { strncpy(name_out, s_name, cap - 1); name_out[cap - 1] = 0; }
  DEBUG_ECU("log: started %s (%u channels @ %u Hz)", s_name, (unsigned)s_nch, s_rate_hz);
  return true;
}

void stop() {
  if (!s_running) return;
  s_running = false;
  vTaskDelay(pdMS_TO_TICKS(150));       // let the writer notice
  // Drain whatever is left ourselves, then close.
  uint8_t chunk[1024];
  xSemaphoreTake(s_file_mtx, portMAX_DELAY);   // writer is between writes now
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  size_t n;
  while ((n = ring_take(chunk, sizeof(chunk))) > 0) { if (s_file) s_file.write(chunk, n); s_bytes += n; }
  free(s_ring); s_ring = nullptr;
  xSemaphoreGive(s_mtx);
  if (s_file) { s_file.flush(); s_file.close(); }
  xSemaphoreGive(s_file_mtx);
  DEBUG_ECU("log: stopped %s — %lu rows, %lu bytes, %lu dropped", s_name,
            (unsigned long)s_rows, (unsigned long)s_bytes, (unsigned long)s_dropped);
}

Status status() {
  Status st{};
  st.running = s_running;
  strncpy(st.file, s_name, sizeof(st.file) - 1);
  st.rows = s_rows; st.bytes = s_bytes; st.dropped = s_dropped;
  st.elapsed_ms = s_running ? millis() - s_started_ms : 0;
  st.fs_used = LittleFS.usedBytes(); st.fs_total = LittleFS.totalBytes();
  return st;
}

void on_frame() {
  if (!s_running || !s_mtx) return;
  uint32_t now = millis();
  uint32_t period = 1000 / s_rate_hz;
  if (s_last_sample_ms && now - s_last_sample_ms < period) return;
  s_last_sample_ms = s_last_sample_ms ? s_last_sample_ms + period : now;
  if (now - s_last_sample_ms > period) s_last_sample_ms = now;   // fell behind: resync

  uint8_t row[4 + 2 * MAX_CHANNELS];
  uint32_t t = now - s_started_ms;
  row[0] = t & 0xFF; row[1] = (t >> 8) & 0xFF; row[2] = (t >> 16) & 0xFF; row[3] = (t >> 24) & 0xFF;
  for (size_t i = 0; i < s_nch; i++) {
    uint16_t raw = 0;
    gauges::raw_channel(s_chs[i], raw);
    row[4 + 2 * i] = raw & 0xFF; row[5 + 2 * i] = raw >> 8;
  }
  size_t n = 4 + 2 * s_nch;
  if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) != pdTRUE) { s_dropped++; return; }
  bool ok = ring_put(row, n);
  xSemaphoreGive(s_mtx);
  if (ok) s_rows++; else s_dropped++;
}

// --- files ------------------------------------------------------------------------
bool remove(const char *name) {
  if (!name || strchr(name, '/') || strstr(name, "..")) return false;
  if (s_running && strcmp(name, s_name) == 0) return false;
  return LittleFS.remove(String(kDir) + "/" + name);
}

size_t remove_all() {
  size_t n = 0;
  String victims[64];
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return 0;
  File f;
  while ((f = dir.openNextFile()) && n < 64) {
    if (!f.isDirectory() && !(s_running && strcmp(f.name(), s_name) == 0)) victims[n++] = f.name();
    f.close();
  }
  dir.close();
  for (size_t i = 0; i < n; i++) LittleFS.remove(String(kDir) + "/" + victims[i]);
  return n;
}

void list(ListFn fn, void *ctx) {
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) fn(f.name(), f.size(), ctx);
    f.close();
  }
}

void begin() {
  s_prefs_open = s_prefs.begin("log", false);
  if (s_prefs_open) {
    if (s_prefs.isKey("sel")) s_prefs.getBytes("sel", s_sel, sizeof(s_sel));
    s_rate_hz = s_prefs.getUChar("rate", 10);
    if (s_rate_hz < 1 || s_rate_hz > 20) s_rate_hz = 10;
  }
  s_mtx = xSemaphoreCreateMutex();
  s_file_mtx = xSemaphoreCreateMutex();
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  xTaskCreatePinnedToCore(writer_task, "logwr", 4096, nullptr, 1, &s_writer, 0);
  DEBUG_ECU("logger ready: %u channel(s) selected @ %u Hz", (unsigned)selected_count(), s_rate_hz);
}

}  // namespace logger
