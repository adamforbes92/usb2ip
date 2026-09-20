#pragma once

// IgnitronUSB — decoded ECU telemetry (gauge channels).
//
// A small, thread-safe snapshot of the latest decoded engine values. The USB
// host task writes via update()/set(); the web-server task reads via snapshot().
// Channel identities are stable so the web UI can key gauges by index even
// before the raw frame layout is fully reverse-engineered.

#include <cstdint>
#include <cstddef>

namespace ecu {

// Stable gauge channels. Order is the API contract with the web UI — append
// only, never renumber. Extend as captures reveal more channels.
enum Channel : uint8_t {
  CH_RPM = 0,     // engine speed        (rpm)
  CH_MAP,         // manifold pressure   (kPa)
  CH_TPS,         // throttle position   (%)
  CH_CLT,         // coolant temp        (deg C)
  CH_IAT,         // intake air temp     (deg C)
  CH_LAMBDA,      // lambda              (ratio)
  CH_BATT,        // battery voltage     (V)
  CH_INJ_DUTY,    // injector duty       (%)
  CH_IGN_ADV,     // ignition advance    (deg BTDC)
  CH_OIL_PRESS,   // oil pressure        (Bar) — Ignitron ch 98
  CH_FUEL_PRESS,  // fuel rail pressure  (Bar) — Ignitron ch 86
  CH_VSS,         // vehicle speed       (km/h)
  CH_TPP,         // accel pedal position (%) — E-Gas driver demand, distinct from CH_TPS
  CH_COUNT
};

struct ChannelMeta {
  const char *key;    // short id used in JSON + UI (e.g. "rpm")
  const char *label;  // human label (e.g. "Engine Speed")
  const char *unit;   // display unit  (e.g. "rpm")
  float min;          // gauge range min
  float max;          // gauge range max
};

// Static metadata for each channel (ranges are sane defaults; tune later).
const ChannelMeta &channel_meta(Channel ch);

// ECU identity — the bootloader/firmware versions of the three processors
// Ignitron's "Firmware manager" lists (COM = USB comms, DSP = knock/lambda,
// CPU = engine). Decoded from the session-start E2 identity reads (see
// ecu_tap.cpp), whether the PC or the bridge itself issued them.
struct EcuUnitVersion {
  bool valid;
  uint8_t bl_major, bl_minor;   // bootloader
  uint8_t fw_major, fw_minor;   // firmware
};
struct EcuIdent {
  EcuUnitVersion com, dsp, cpu;
};
enum EcuUnit : uint8_t { UNIT_COM = 0, UNIT_DSP, UNIT_CPU };

// A complete decoded snapshot.
struct TelemetryState {
  float values[CH_COUNT];    // decoded value per channel
  bool valid[CH_COUNT];      // has this channel ever been decoded?
  uint32_t frame_count;      // total frames decoded since boot
  uint32_t last_update_ms;   // millis() of last successful decode
  bool link_ok;              // is fresh data arriving?
  EcuIdent ident;
};

// Thread-safe store. Single global instance shared across tasks.
class TelemetryStore {
 public:
  void begin();

  // Writers (USB task / mock source).
  void set(Channel ch, float value);
  void set_ecu_version(EcuUnit unit, bool firmware, uint8_t major, uint8_t minor);
  void mark_frame();  // call once per successfully decoded frame

  // Reader (web task). Copies the current state under lock.
  TelemetryState snapshot();

 private:
  void refresh_link_(uint32_t now_ms);

  TelemetryState state_{};
  void *mutex_{nullptr};  // SemaphoreHandle_t (opaque to avoid FreeRTOS in hdr)
};

// Global telemetry store.
extern TelemetryStore g_telemetry;

// Initialize the telemetry store before ECU frame processing starts.
void begin();

}  // namespace ecu
