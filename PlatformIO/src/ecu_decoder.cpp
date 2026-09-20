#include "ecu_decoder.h"

#include <Arduino.h>
#include <cmath>

#include "defs.h"
#include "gauges.h"

namespace ecu {

// ===========================================================================
// FRAME LAYOUT — see ecu_decoder.h. Nothing here is fitted: the registry's
// rows carry the channel index, and gauges::decode_frame() knows that the
// 64 B block is channels 0..31 and the 704 B block channels 32..383, both
// as u16 little-endian in channel order. The 8 B transfer that precedes each
// block is the ECU's reply header and carries no channel data.
// ===========================================================================

int Decoder::feed(const uint8_t *data, size_t len) {
  if (!gauges::decode_frame(data, len)) return 0;   // not a channel block
  // One "frame" per main block: the 64 B DSP block arrives right after it in
  // the same poll cycle (that's the order Ignitron's logger snapshots them
  // in), so counting both would double the frame rate.
  if (len == gauges::MAIN_BLOCK_LEN) g_telemetry.mark_frame();
  return 1;
}

// ---------------------------------------------------------------------------
// Mock telemetry: an animated idle + throttle-blip so the web gauges can be
// built and demoed without an ECU.
// ---------------------------------------------------------------------------
void mock_tick() {
  float t = millis() / 1000.0f;
  float blip = 0.5f * (1.0f + sinf(t * 0.6f));            // 0..1 slow sweep
  float rpm = 850.0f + blip * 5200.0f + 40.0f * sinf(t * 9.0f);
  float tps = blip * 92.0f;
  float map = 32.0f + blip * 165.0f;
  float clt = 88.0f + 3.0f * sinf(t * 0.2f);
  float iat = 34.0f + 2.0f * sinf(t * 0.15f);
  float lambda = 1.0f - 0.18f * blip + 0.02f * sinf(t * 3.0f);
  float batt = 14.1f - 0.4f * blip;

  g_telemetry.set(CH_RPM, rpm);
  g_telemetry.set(CH_TPS, tps);
  g_telemetry.set(CH_MAP, map);
  g_telemetry.set(CH_CLT, clt);
  g_telemetry.set(CH_IAT, iat);
  g_telemetry.set(CH_LAMBDA, lambda);
  g_telemetry.set(CH_BATT, batt);
  g_telemetry.set(CH_INJ_DUTY, 2.0f + blip * 65.0f);
  g_telemetry.set(CH_IGN_ADV, 12.0f + blip * 22.0f);
  g_telemetry.set(CH_OIL_PRESS, 1.2f + blip * 3.8f);
  g_telemetry.set(CH_FUEL_PRESS, 3.0f + blip * 0.2f);
  g_telemetry.set(CH_VSS, blip * blip * 180.0f);
  g_telemetry.set(CH_TPP, blip * 95.0f);
  g_telemetry.mark_frame();
}

}  // namespace ecu
