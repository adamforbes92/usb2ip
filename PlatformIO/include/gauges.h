#pragma once

// IgnitronUSB — gauge registry.
//
// One table (include/gauges_table.h, generated) describes every channel the
// ECU streams: its index, how to scale it, what to call it, and which of the
// six UI groups it belongs to. The dashboard renders whichever gauges the
// user has switched on; the Gauges tab is just a view of this table.
//
// Where the numbers come from — not curve fitting: the Ignitron tuning
// software carries the whole catalogue inside Ignitron.exe (a string table
// with one name+unit per channel and a float table with each channel's
// decimals/multiplier/offset/range), and tools/ignitron_exe_extract.py pulls
// it out verbatim. tools/verify_map.py then proved, on a simultaneous
// Ignitron .ilf log + USB capture, that every one of the 384 channels is a
// u16 at a fixed slot of the two bulk-IN blocks the ECU answers the poll with:
//
//     64 B transfer  = channels   0..31   (DSP: knock / lambda controller)
//    704 B transfer  = channels  32..383  (everything else)
//
// so a GaugeDef needs only the channel index; the frame and byte offset
// follow from it (see frame_slot()).

#include <cstdint>
#include <cstddef>

namespace gauges {

// Two independent taxonomies, because they answer different questions:
//
//   Group — "where do I find this gauge to switch it on": the six dropdowns
//           on the Gauges tab (2x3), organised by what the channel measures.
//   Type  — "where does it sit once it's on": the dashboard's optional
//           "Arrange as Type" sections, organised by what you'd be looking at
//           it for. Basic is the everyday set; the rest group the tuning
//           channels; Advanced catches everything else.
enum Group : uint8_t {
  GRP_ENGINE = 0,
  GRP_AIRFUEL,
  GRP_TEMPS,
  GRP_ELECTRICAL,
  GRP_VEHICLE,
  GRP_DIAG,
  GRP_COUNT
};

enum Type : uint8_t {
  TYPE_BASIC = 0,
  TYPE_IGNITION,
  TYPE_KNOCK,
  TYPE_INJECTION,
  TYPE_ADVANCED,
  TYPE_COUNT
};

// Channel sentinel for "the ECU never fills this slot in" — four log columns
// (32 AFR, 381 USB timing, 382/383 timestamp) are computed by the PC.
constexpr uint16_t CH_NOT_SENT = 0xFFFF;
constexpr uint16_t CH_COUNT_TOTAL = 384;
constexpr uint16_t DSP_BLOCK_CHANNELS = 32;   // 64 B block = channels 0..31
constexpr size_t   DSP_BLOCK_LEN = 64;
constexpr size_t   MAIN_BLOCK_LEN = 704;

// The catalogue is 384 channels; the dashboard is not. Cap how many can be
// on at once — beyond this the dials are unreadable anyway, and it bounds the
// per-frame decode work and the /api/status payload.
constexpr size_t MAX_ENABLED = 16;

struct GaugeDef {
  const char *key;      // stable id used in JSON + NVS (e.g. "tps")
  const char *label;    // Ignitron's own channel name
  const char *unit;     // display unit (UTF-8, e.g. "\xC2\xB0""C")
  uint16_t ch;          // ECU channel index 0..383, or CH_NOT_SENT
  bool is_signed;       // raw is int16 rather than uint16
  float scale;          // engineering = raw * scale + bias
  float bias;
  float dmin, dmax;     // gauge face range (engineering units)
  uint8_t dp;           // decimals to print (what Ignitron's viewer shows)
  uint8_t group;        // Group — which Gauges-tab dropdown lists it
  uint8_t type;         // Type — which dashboard section it sits in
  bool default_on;      // shown on the dashboard out of the box
  int8_t legacy;        // ecu::Channel to mirror into TelemetryStore, or -1
};

size_t count();
const GaugeDef &def(size_t i);
int index_of(const char *key);          // -1 if unknown
int index_of_channel(uint16_t ch);      // table row carrying ECU channel ch, or -1

// Switch every gauge off without touching NVS (bulk import: caller then
// enables its list, which persists).
void clear_enabled();

bool mapped(size_t i);                  // carried in the stream -> can be decoded
bool enabled(size_t i);
size_t enabled_count();

// Persists to NVS. Returns false if the change was refused: switching a gauge
// on when MAX_ENABLED are already on, or one the ECU doesn't send.
bool set_enabled(size_t i, bool on);

float value(size_t i);                  // last decoded engineering value
bool valid(size_t i);                   // has it ever decoded?

const char *group_name(uint8_t g);
const char *type_name(uint8_t t);

// Dashboard layout preference: group the enabled gauges into Type sections
// rather than one flat grid. Persisted alongside the enable-state.
bool arrange_by_type();
void set_arrange_by_type(bool on);

// Dashboard preferences, persisted on the board so every browser that opens
// the page sees the same dashboard.
bool sweep_on_launch();  void set_sweep_on_launch(bool on);   // needle sweep when the ECU connects
bool auto_launch();      void set_auto_launch(bool on);       // "Launch Gauges" as soon as the page opens
const char *hero_key();  bool set_hero_key(const char *key);  // which gauge owns the big top dial
const char *order();     void set_order(const char *csv);     // flat-grid order, comma-separated keys

// Load persisted enable-state. Call once from setup(), after Preferences is up.
void begin();

// Which bulk-IN block a channel lives in and its byte offset there.
// Returns false for CH_NOT_SENT / out-of-range.
bool frame_slot(uint16_t ch, size_t &block_len, size_t &byte_offset);

// Raw u16 of any channel from the most recent block that carried it, whether
// or not a gauge for it is enabled. Used for status words the UI always needs
// (fault counts, last fault code, limp-mode bit) without burning gauge slots.
// Returns false until that block has been received at least once.
bool raw_channel(uint16_t ch, uint16_t &raw);

// Decode every mapped+enabled gauge (plus the legacy-telemetry ones) out of
// one bulk-IN transfer. Accepts the 64 B and the 704 B block; anything else
// is ignored. Returns true if the transfer was one of the two.
bool decode_frame(const uint8_t *frame, size_t len);

}  // namespace gauges
