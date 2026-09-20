#include "gauges.h"
#include "logger.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "defs.h"
#include "telemetry.h"

namespace gauges {

// ===========================================================================
// THE TABLE — generated, not hand-written.
//
// tools/ignitron_exe_extract.py reads the channel catalogue out of the
// installed Ignitron.exe (names, units, decimals, multiplier, offset, range
// for all 384 channels, plus the 303 status-bit definitions),
// tools/make_channel_map.py adds the UI opinions (keys, groups, defaults) and
// tools/gen_gauges.py writes gauges_table.h. tools/verify_map.py checks the
// result against a real session: with session1.bin + the matching .ilf log,
// all 144 live channels decode bit-identically to what Ignitron logged, the
// 236 constant ones hold the same constant, and the 4 PC-computed columns
// are the only slots the ECU leaves at zero.
// ===========================================================================
#include "gauges_table.h"
static constexpr size_t kCount = sizeof(kGauges) / sizeof(kGauges[0]);

static const char *kGroupNames[GRP_COUNT] = {
    "Engine", "Air & Fuel", "Temperatures", "Electrical", "Vehicle", "Diagnostics",
};

static const char *kTypeNames[TYPE_COUNT] = {
    "Basic", "Ignition", "Knock", "Injection", "Advanced",
};

// --- runtime state ---------------------------------------------------------
static float s_value[kCount] = {0};
static bool s_valid[kCount] = {false};
// Every channel's last raw word (768 B) — cheap, and it means status fields
// don't depend on which gauges the user has switched on.
static uint16_t s_raw[CH_COUNT_TOTAL] = {0};
static bool s_raw_valid[2] = {false, false};   // [0] DSP block, [1] main block
static uint8_t s_enabled[(kCount + 7) / 8] = {0};
static bool s_arrange_by_type = true;
static Preferences s_prefs;
static bool s_prefs_open = false;
// The enable bitmap is by table index, so it is only meaningful for the table
// it was saved against. Bumped when the table's row order changed (v2: rows
// are now the ECU's channel order, 0..383).
static const char *kEnableKey = "gaugeEn2";
static bool s_sweep = true;
static bool s_auto_launch = false;
static char s_hero[40] = "rpm";
static char s_order[640] = "";   // 16 keys x up to 39 chars

static inline bool bit_get(const uint8_t *bits, size_t i) {
  return (bits[i >> 3] >> (i & 7)) & 1;
}
static inline void bit_set(uint8_t *bits, size_t i, bool on) {
  if (on) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
  else    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

size_t count() { return kCount; }
const GaugeDef &def(size_t i) { return kGauges[i < kCount ? i : 0]; }
bool mapped(size_t i) { return i < kCount && kGauges[i].ch != CH_NOT_SENT; }
bool enabled(size_t i) { return i < kCount && bit_get(s_enabled, i); }
float value(size_t i) { return i < kCount ? s_value[i] : 0.0f; }
bool valid(size_t i) { return i < kCount && s_valid[i]; }
const char *group_name(uint8_t g) { return g < GRP_COUNT ? kGroupNames[g] : "?"; }
const char *type_name(uint8_t t) { return t < TYPE_COUNT ? kTypeNames[t] : "?"; }
bool arrange_by_type() { return s_arrange_by_type; }

int index_of(const char *key) {
  if (!key) return -1;
  for (size_t i = 0; i < kCount; i++)
    if (strcmp(kGauges[i].key, key) == 0) return (int)i;
  return -1;
}

static void persist() {
  if (!s_prefs_open) return;
  s_prefs.putBytes(kEnableKey, s_enabled, sizeof(s_enabled));
}

size_t enabled_count() {
  size_t n = 0;
  for (size_t i = 0; i < kCount; i++) if (bit_get(s_enabled, i)) n++;
  return n;
}

int index_of_channel(uint16_t ch) {
  if (ch < kCount && kGauges[ch].ch == ch) return (int)ch;   // rows are in channel order
  for (size_t i = 0; i < kCount; i++) if (kGauges[i].ch == ch) return (int)i;
  return -1;
}

void clear_enabled() { memset(s_enabled, 0, sizeof(s_enabled)); }

bool set_enabled(size_t i, bool on) {
  if (i >= kCount) return false;
  if (on) {
    if (bit_get(s_enabled, i)) return true;                 // already on
    if (kGauges[i].ch == CH_NOT_SENT) return false;         // nothing to decode
    if (enabled_count() >= MAX_ENABLED) return false;       // dashboard is full
  }
  bit_set(s_enabled, i, on);
  persist();
  return true;
}

void set_arrange_by_type(bool on) {
  s_arrange_by_type = on;
  if (s_prefs_open) s_prefs.putBool("gaugeArr", on);
}

bool sweep_on_launch() { return s_sweep; }
void set_sweep_on_launch(bool on) {
  s_sweep = on;
  if (s_prefs_open) s_prefs.putBool("gaugeSweep", on);
}

bool auto_launch() { return s_auto_launch; }
void set_auto_launch(bool on) {
  s_auto_launch = on;
  if (s_prefs_open) s_prefs.putBool("gaugeAuto", on);
}

const char *hero_key() { return s_hero; }
bool set_hero_key(const char *key) {
  if (!key || index_of(key) < 0 || strlen(key) >= sizeof(s_hero)) return false;
  strncpy(s_hero, key, sizeof(s_hero) - 1);
  s_hero[sizeof(s_hero) - 1] = 0;
  if (s_prefs_open) s_prefs.putString("gaugeHero", s_hero);
  return true;
}

const char *order() { return s_order; }
void set_order(const char *csv) {
  if (!csv) csv = "";
  strncpy(s_order, csv, sizeof(s_order) - 1);
  s_order[sizeof(s_order) - 1] = 0;
  if (s_prefs_open) s_prefs.putString("gaugeOrder", s_order);
}

void begin() {
  // Defaults first, so a table that grew since the last save still gets sane
  // state for the new rows rather than inheriting a zero bit.
  for (size_t i = 0; i < kCount; i++) bit_set(s_enabled, i, kGauges[i].default_on);

  s_prefs_open = s_prefs.begin("ignitron", false);
  if (!s_prefs_open) {
    DEBUG_ECU("gauges: NVS unavailable, using defaults");
    return;
  }
  s_arrange_by_type = s_prefs.getBool("gaugeArr", true);
  s_sweep = s_prefs.getBool("gaugeSweep", true);
  s_auto_launch = s_prefs.getBool("gaugeAuto", false);
  s_prefs.getString("gaugeHero", s_hero, sizeof(s_hero));
  if (!s_hero[0] || index_of(s_hero) < 0) strcpy(s_hero, "rpm");
  s_prefs.getString("gaugeOrder", s_order, sizeof(s_order));
  uint8_t stored[sizeof(s_enabled)] = {0};
  size_t got = s_prefs.getBytes(kEnableKey, stored, sizeof(stored));
  if (got == sizeof(s_enabled)) {
    memcpy(s_enabled, stored, sizeof(s_enabled));
  } else if (got > 0) {
    // Table grew: keep what was saved, leave the new rows at their defaults.
    memcpy(s_enabled, stored, got);
  }
  // Trim anything the table/CSV or an older save left over the cap, and drop
  // gauges the ECU doesn't send, so the invariant holds however we got here.
  size_t kept = 0;
  for (size_t i = 0; i < kCount; i++) {
    if (!bit_get(s_enabled, i)) continue;
    if (kGauges[i].ch == CH_NOT_SENT || kept >= MAX_ENABLED)
      bit_set(s_enabled, i, false);
    else
      kept++;
  }
  DEBUG_ECU("gauges: %u defined, %u enabled (max %u)",
            (unsigned)kCount, (unsigned)kept, (unsigned)MAX_ENABLED);
}

bool raw_channel(uint16_t ch, uint16_t &raw) {
  if (ch >= CH_COUNT_TOTAL) return false;
  if (!s_raw_valid[ch < DSP_BLOCK_CHANNELS ? 0 : 1]) return false;
  raw = s_raw[ch];
  return true;
}

bool frame_slot(uint16_t ch, size_t &block_len, size_t &byte_offset) {
  if (ch >= CH_COUNT_TOTAL) return false;
  if (ch < DSP_BLOCK_CHANNELS) {
    block_len = DSP_BLOCK_LEN;
    byte_offset = (size_t)ch * 2;
  } else {
    block_len = MAIN_BLOCK_LEN;
    byte_offset = (size_t)(ch - DSP_BLOCK_CHANNELS) * 2;
  }
  return true;
}

bool decode_frame(const uint8_t *frame, size_t len) {
  // Which channel range does this transfer carry?
  uint16_t first, last;
  if (len == DSP_BLOCK_LEN) {
    first = 0; last = DSP_BLOCK_CHANNELS - 1;
  } else if (len == MAIN_BLOCK_LEN) {
    first = DSP_BLOCK_CHANNELS; last = CH_COUNT_TOTAL - 1;
  } else {
    return false;
  }
  for (uint16_t ch = first; ch <= last; ch++)
    s_raw[ch] = (uint16_t)(frame[(ch - first) * 2] | (frame[(ch - first) * 2 + 1] << 8));
  s_raw_valid[first == 0 ? 0 : 1] = true;
  if (first != 0) logger::on_frame();   // one sample per poll cycle, after the main block

  for (size_t i = 0; i < kCount; i++) {
    const GaugeDef &g = kGauges[i];
    if (g.ch < first || g.ch > last) continue;             // other block / not sent
    if (!bit_get(s_enabled, i) && g.legacy < 0) continue;  // hidden: skip the work
    const uint8_t *p = frame + (size_t)(g.ch - first) * 2;
    uint16_t raw = (uint16_t)(p[0] | (p[1] << 8));         // every channel is u16 LE
    float v = g.is_signed ? (float)(int16_t)raw * g.scale + g.bias
                          : (float)raw * g.scale + g.bias;
    s_value[i] = v;
    s_valid[i] = true;
    if (g.legacy >= 0) ecu::g_telemetry.set((ecu::Channel)g.legacy, v);
  }
  return true;
}

}  // namespace gauges
