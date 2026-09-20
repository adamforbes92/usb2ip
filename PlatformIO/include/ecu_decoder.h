#pragma once

// IgnitronUSB — ECU gauge-frame decoder.
//
// The ECU's bulk-IN endpoint carries fixed-size messages, one per transfer,
// in answer to the tuning software's vendor control poll (bRequest 0xFA):
//
//   wValue=2, data[0]=0x09 ->  8 B header + 64 B block   (channels   0..31)
//   wValue=3, data[0]=0x59 ->  8 B header + 704 B block  (channels  32..383)
//
// (data[0] is the transfer length in 8-byte words, header included: 9*8 = 72,
// 89*8 = 712.) Both blocks are plain arrays of little-endian u16, one per
// channel, in the same channel order Ignitron's .ilf logs use — proved by
// tools/verify_map.py on a simultaneous log + capture. There is no sync word
// and no checksum; the transfer boundary is the framing.
//
// Everything about the channels themselves (names, units, scaling) lives in
// the gauge registry (gauges.h). This file just routes each completed bulk
// transfer to it and keeps the legacy TelemetryStore fed.

#include <cstdint>
#include <cstddef>
#include "telemetry.h"

namespace ecu {

// Stateful decoder: feed it each completed bulk-IN transfer.
class Decoder {
 public:
  // Decode a transfer if it is one of the two channel blocks. Returns 1 if it
  // was (and the telemetry frame counter was bumped), else 0.
  int feed(const uint8_t *data, size_t len);

  // The layout is known; kept for callers that gate on it.
  static bool configured() { return true; }
};

// Fill g_telemetry with plausible, animated engine values for UI development.
// Call periodically (e.g. from the web/telemetry task) when ECU_MOCK is set.
void mock_tick();

}  // namespace ecu
