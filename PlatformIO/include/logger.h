#pragma once

// IgnitronUSB — data logger.
//
// Logs any subset of the ECU's 384 live channels, independent of the
// dashboard's 16-dial display cap. Two ways to use it:
//
//   * ON THE DEVICE: start()/stop() write a compact binary log to LittleFS
//     (/logs/log_NNNN.ilg). Rows are appended through a ring buffer by a
//     low-priority writer task, so the USB poll never waits on flash, and the
//     file is flushed every couple of seconds — a power cut costs at most that.
//     Downloads convert to CSV on the fly (see csv_stream.h) or hand over the
//     raw file.
//   * IN THE BROWSER: the page polls /api/log/sample at the chosen rate and
//     accumulates rows itself; see data/app.js. Nothing here but sample().
//
// .ilg layout (all little-endian):
//   "IGLG" | u8 version=1 | u8 rate_hz | u16 nch | u32 reserved | u16 ch[nch]
//   then rows of: u32 t_ms | u16 raw[nch]          (raw = ECU u16 as sent)
// Engineering values are recovered from the gauge registry's scale/bias, so
// a log stays decodable even if the table is regenerated.

#include <cstddef>
#include <cstdint>

namespace logger {

constexpr size_t MAX_CHANNELS = 384;      // per log (the whole catalogue); row = 4 + 2*n bytes
constexpr uint8_t kVersion = 1;
static const char *const kDir = "/logs";

void begin();                              // load config from NVS, create /logs

// --- configuration (persisted) --------------------------------------------
size_t channels(uint16_t *out, size_t cap);            // selected, ascending
bool set_channels(const uint16_t *chs, size_t n);      // n <= MAX_CHANNELS
uint8_t rate_hz();
void set_rate_hz(uint8_t hz);                          // 1..20

// --- device logging ---------------------------------------------------------
// name: optional, sanitised to [A-Za-z0-9_-] (max 24) and made unique with a
// numeric suffix if it already exists; empty -> log_NNNN. name_out gets the
// final file name.
bool start(const char *name, char *name_out, size_t cap);   // false: running / no channels / fs
void stop();
bool running();

struct Status {
  bool running;
  char file[32];
  uint32_t rows;
  uint32_t bytes;
  uint32_t elapsed_ms;
  uint32_t dropped;      // rows lost to a full ring buffer (writer starved)
  size_t fs_used, fs_total;
};
Status status();

// Called by the gauge decoder each time the main block lands. Samples at
// rate_hz while a log is running. Cheap when idle.
void on_frame();

// --- files ------------------------------------------------------------------
bool remove(const char *name);            // basename, e.g. "log_0003.ilg"
size_t remove_all();                      // every stored log except one being written; returns count

// Fill a JSON-ish listing via callback (name, bytes) to avoid a big String.
typedef void (*ListFn)(const char *name, size_t bytes, void *ctx);
void list(ListFn fn, void *ctx);

}  // namespace logger
