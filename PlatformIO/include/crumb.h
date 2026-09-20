#pragma once

// Crash breadcrumbs. A phase marker kept in RTC slow memory, which survives
// every reset except a power cycle. Each long-running loop stamps the phase
// it is entering; after an abnormal reset (panic, watchdog, brownout) boot
// prints the last stamp, so a death that produced no panic output still
// says roughly where the firmware was. Cost: one store per stamp.
#include <cstdint>

namespace crumb {

enum Phase : uint8_t {
  NONE = 0,
  BOOT,
  BRIDGE_DEVICE,     // usb device connect/disconnect handling
  BRIDGE_CLIENT,     // USB/IP socket accept + request parsing
  BRIDGE_POLL,       // pending-URB completion loop
  BRIDGE_SUBMIT,     // handing a URB to the USB host stack
  BRIDGE_INTERCEPT,  // SET_CONFIGURATION / SET_INTERFACE / CLEAR_FEATURE rebuild
  PORT_LOOP,         // routing state machine + LED
  WEB_STATUS,        // /api/status JSON
  WEB_SCAN,          // band scan
  STATUS_TASK,       // 1 Hz telemetry line
};

void set(Phase p);            // stamp the current phase
void report_after_reset();    // call once at boot, after Serial is up

}  // namespace crumb
