#include "crumb.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

#include "defs.h"

namespace crumb {

// RTC_NOINIT: not zeroed on reset, not initialised by the loader.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint8_t s_phase;
static RTC_NOINIT_ATTR uint32_t s_uptime_ms;
static constexpr uint32_t kMagic = 0xC7A5B001;

static const char *name(uint8_t p) {
  switch (p) {
    case BOOT:             return "boot";
    case BRIDGE_DEVICE:    return "bridge:device";
    case BRIDGE_CLIENT:    return "bridge:client";
    case BRIDGE_POLL:      return "bridge:poll";
    case BRIDGE_SUBMIT:    return "bridge:submit";
    case BRIDGE_INTERCEPT: return "bridge:intercept";
    case PORT_LOOP:        return "port:loop";
    case WEB_STATUS:       return "web:status";
    case WEB_SCAN:         return "web:scan";
    case STATUS_TASK:      return "status-task";
    default:               return "none";
  }
}

void set(Phase p) {
  s_phase = p;
  s_uptime_ms = millis();
}

void report_after_reset() {
  esp_reset_reason_t r = esp_reset_reason();
  bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                   r == ESP_RST_WDT || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN);
  if (s_magic == kMagic && abnormal) {
    DEBUG("last crumb before that reset: %s (uptime %lus)", name(s_phase),
          (unsigned long)(s_uptime_ms / 1000));
  }
  s_magic = kMagic;
  s_phase = BOOT;
  s_uptime_ms = 0;
}

}  // namespace crumb
