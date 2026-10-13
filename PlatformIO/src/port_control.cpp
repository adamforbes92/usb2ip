#include "port_control.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#include "defs.h"
#include "ignitron_wifi.h"
#include "ota_manager.h"
#include "usbip_bridge.h"

namespace port {

PortController g_port;

static Preferences s_prefs;

// OC# falling edges. The TPS2041B current-limits and thermal-cycles, so OC#
// can pulse low for only ~10s of ms — shorter than the 50 ms poll. The ISR
// counts every assertion so none goes unreported.
static volatile uint32_t s_oc_edges = 0;
static void IRAM_ATTR oc_isr() { s_oc_edges++; }

// --- low-level pin actions ------------------------------------------------

void PortController::ecu_power(bool on) {
  if (on == ecu_powered_) return;
  if (on) {
    pinMode(PIN_IGNITRON_PWR, OUTPUT);
    digitalWrite(PIN_IGNITRON_PWR, LOW);   // EN# low → TPS2041B on
  } else {
    pinMode(PIN_IGNITRON_PWR, INPUT);      // Hi-Z → R19 pulls EN# high → off
  }
  ecu_powered_ = on;
  DEBUG_PWR("ECU 5V %s", on ? "ON" : "OFF");
}

void PortController::mux(MuxMode mode) {
  // Break-before-make: disconnect (Hi-Z) before changing SEL so neither host
  // sees a half-switched bus.
  digitalWrite(PIN_USB_OE, HIGH);
  if (mode == MuxMode::ESP_HOST) {
    digitalWrite(PIN_USB_SEL, MUX_SEL_ESP);
    delay(2);
    digitalWrite(PIN_USB_OE, LOW);
  } else if (mode == MuxMode::LAPTOP) {
    digitalWrite(PIN_USB_SEL, MUX_SEL_LAPTOP);
    delay(2);
    digitalWrite(PIN_USB_OE, LOW);
  }
  // MuxMode::OFF leaves /OE HIGH → mux Hi-Z (USB-A floating).
  mux_ = mode;
}

void PortController::wifi(bool on) {
  if (on) wifiBridgeStart();
  else    wifiBridgeStop();
}

bool PortController::compute_wifi_in_use() {
  if (!wifiBridgeActive()) return false;
  // An OTA upload must never be cut by the idle watchdog: dropping the radio
  // (or the ECU rail) part-way through a flash write leaves a half-written
  // partition. This is the ota_manager powerIsBusy() hook for this project.
  if (otaInProgress()) return true;
  if (WiFi.softAPgetStationNum() > 0) return true;
  // A phone working through the home router (Home WiFi bridge mode) is not an
  // AP station - any web request in the last 30 s counts as in use.
  if (otaWebClientActive()) return true;
  return usbip::g_usbip_component && usbip::g_usbip_component->client_attached();
}

bool PortController::read_vbus() {
  vbus_raw_ = digitalRead(PIN_VBUS_SENSE) == HIGH;
  return vbus_raw_;
}

// TPS2041B OC# (active-low). Only meaningful while the switch is enabled.
void PortController::check_fault() {
  uint32_t edges = s_oc_edges;
  if (edges != fault_count_) {
    uint32_t n = edges - fault_count_;
    fault_count_ = edges;
    last_fault_ms_ = millis();
    DEBUG_PWR("ECU 5V over-current event%s (+%lu, %lu since boot)",
              n > 1 ? "s" : "", (unsigned long)n, (unsigned long)edges);
  }
  bool f = ecu_powered_ && digitalRead(PIN_IGNITRON_OC) == LOW;
  if (f != fault_) {
    fault_ = f;
    if (f) DEBUG_PWR("ECU 5V FAULT — TPS2041B OC# low (over-current / thermal limit on USB-A)");
    else   DEBUG_PWR("ECU 5V fault cleared");
  }
}

// --- stage transitions ----------------------------------------------------

// Power the port and route it. The destination is the forced route if one is
// pinned, otherwise the laptop when USB-C is present, else the ESP host.
// Power first, then data — the same order a real plug makes contact in.
void PortController::enter_active(uint32_t now, bool usbc) {
  bool to_laptop = route_mode_ == RouteMode::USBC ||
                   (route_mode_ == RouteMode::AUTO && usbc);
  asleep_ = false;
  ecu_power(true);
  if (to_laptop) {
    mux(MuxMode::LAPTOP);
    if (cfg_.usbc_disables_wifi) wifi(false);
  } else {
    mux(MuxMode::ESP_HOST);
    wifi(true);
  }
  last_activity_ms_ = now;   // arm / restart the 5-minute idle window
}

void PortController::enter_active_esp(uint32_t now) {
  asleep_ = false;
  ecu_power(true);
  mux(MuxMode::ESP_HOST);
  last_activity_ms_ = now;   // arm / restart the 5-minute idle window
}

// Standby: USB-A unpowered and disconnected; the AP stays up so a WiFi client
// can wake it. Data first, then power — the order a real unplug breaks in.
void PortController::enter_standby(const char *why) {
  DEBUG_PWR("standby (%s) — ECU 5V off, USB-A open, waiting for USB-C or a WiFi client", why);
  mux(MuxMode::OFF);
  ecu_power(false);
  asleep_ = true;
  fault_ = false;
}

// --- lifecycle ------------------------------------------------------------

void PortController::begin() {
  s_prefs.begin("ignitron", false);
  cfg_.usbc_disables_wifi     = s_prefs.getBool("cDisWifi", DEFAULT_USBC_DISABLES_WIFI);
  cfg_.restore_wifi_on_unplug = s_prefs.getBool("cResWifi", DEFAULT_RESTORE_WIFI_ON_UNPLUG);
  cfg_.idle_auto_off          = s_prefs.getBool("idleOff", DEFAULT_IDLE_AUTO_OFF);
  route_mode_ = (RouteMode)s_prefs.getUChar("route", (uint8_t)RouteMode::AUTO);
  if (!s_prefs.isKey("route"))
    DEBUG_PWR("NVS has no routing override (factory state) — using AUTO");

  pinMode(PIN_ESP_LED, OUTPUT);
  digitalWrite(PIN_ESP_LED, LOW);
  pinMode(PIN_VBUS_SENSE, INPUT);
  pinMode(PIN_IGNITRON_OC, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_IGNITRON_OC), oc_isr, FALLING);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  pinMode(PIN_IGNITRON_PWR, INPUT);          // EN# Hi-Z → port off
  ecu_powered_ = false;
  pinMode(PIN_USB_OE, OUTPUT);
  digitalWrite(PIN_USB_OE, HIGH);            // start disconnected
  pinMode(PIN_USB_SEL, OUTPUT);
  digitalWrite(PIN_USB_SEL, MUX_SEL_ESP);
  mux_ = MuxMode::OFF;

  bool usbc = read_vbus();
  usbc_prev_ = usbc;
  vbus_filter_ = usbc ? 4 : 0;

  uint32_t now = millis();
  last_poll_ms_ = now;
  led_last_ms_ = now;
  DEBUG_PWR("port control up — vbus=%d, route=%d (0 auto, 1 usbc, 2 wifi)",
            usbc, (int)route_mode_);

  // Never power the port just because the board powered up. A USB-C host
  // already present at boot IS a request (unless WiFi is the pinned route);
  // otherwise wait in standby for loop() to see USB-C or a WiFi client.
  if (usbc && route_mode_ != RouteMode::WIFI) {
    DEBUG_PWR("USB-C present at boot → port on");
    enter_active(now, true);
  } else {
    enter_standby("boot");
  }
}

void PortController::loop() {
  uint32_t now = millis();
  update_led(now);

  if (now - last_poll_ms_ < 50) return;     // poll VBUS / timer at 20 Hz
  last_poll_ms_ = now;

  check_boot_button(now);
  check_fault();

  // Debounced USB-C presence (needs 4 consistent samples to flip either way).
  // Always tracked for the status page; only acted on in AUTO (+ standby wake
  // for a pinned USB-C route).
  bool raw = read_vbus();
  if (raw && vbus_filter_ < 4) vbus_filter_++;
  if (!raw && vbus_filter_ > 0) vbus_filter_--;
  bool usbc = usbc_prev_;
  if (vbus_filter_ >= 4) usbc = true;
  if (vbus_filter_ == 0) usbc = false;
  bool inserted = usbc && !usbc_prev_;
  bool removed  = !usbc && usbc_prev_;
  usbc_prev_ = usbc;

  if (route_mode_ == RouteMode::AUTO) {
    if (inserted) {
      // Stage 2 — USB-C inserted.
      DEBUG_PWR("USB-C connected → port on, mux to laptop%s",
                cfg_.usbc_disables_wifi ? ", WiFi bridge off" : "");
      enter_active(now, true);
    } else if (removed) {
      // Stage 3 — USB-C removed. A WiFi client still connected keeps the port
      // powered and takes it over (no power blip); otherwise back to standby.
      DEBUG_PWR("USB-C removed%s", cfg_.restore_wifi_on_unplug ? ", WiFi bridge restored" : "");
      if (cfg_.restore_wifi_on_unplug) wifi(true);
      if (compute_wifi_in_use()) enter_active(now, false);
      else                       enter_standby("USB-C removed");
    }
  }

  wifi_in_use_ = compute_wifi_in_use();

  if (asleep_) {
    // Standby — power up only on a request: a WiFi client, or USB-C (a pinned
    // WiFi route ignores VBUS).
    bool usbc_req = usbc && route_mode_ != RouteMode::WIFI;
    if (wifi_in_use_ || usbc_req) {
      DEBUG_PWR("request (%s) → port on", usbc_req ? "USB-C" : "WiFi client");
      enter_active(now, usbc);
    }
    return;
  }

  // Forced route, or laptop present → hold powered, never auto power-down.
  if (route_mode_ != RouteMode::AUTO || usbc) {
    last_activity_ms_ = now;
    return;
  }

  // Stage 4 — idle watchdog. Disabled → hold the ECU powered indefinitely.
  if (!cfg_.idle_auto_off) {
    last_activity_ms_ = now;
    return;
  }
  if (wifi_in_use_) last_activity_ms_ = now;
  if (now - last_activity_ms_ >= IDLE_SHUTDOWN_MS) enter_standby("idle 5 min");
}

// --- status LED -----------------------------------------------------------
// Patterns are (on, off) millisecond pairs, looped. See defs.h for meanings.
static const uint16_t kLedSlow[]   = {150, 1350};                 // no device
static const uint16_t kLedDouble[] = {120, 150, 120, 1200};       // device idle
static const uint16_t kLedFast[]   = {80, 80};                    // transferring
static const uint16_t kLedStrobe[] = {40, 40};                    // reset armed
static const uint16_t kLedStandby[] = {30, 2970};                 // standby heartbeat

void PortController::update_led(uint32_t now) {
  const uint16_t *pat = nullptr;
  uint8_t len = 0;
  bool solid = false;

  bool dev_present = usbip::g_usbip_component && usbip::g_usbip_component->device_info().present;
  bool xfer = usbip::g_usbip_component && usbip::g_usbip_component->active_within(LED_ACTIVE_MS);
  bool laptop = (mux_ == MuxMode::LAPTOP);

  if (boot_armed_)      { pat = kLedStrobe; len = 2; }
  else if (asleep_)     { pat = kLedStandby; len = 2; }
  else if (laptop)      { solid = true; }
  else if (xfer)        { pat = kLedFast; len = 2; }
  else if (dev_present) { pat = kLedDouble; len = 4; }
  else                  { pat = kLedSlow; len = 2; }

  if (solid) {
    led_pat_ = nullptr;
    if (led_on_ != solid) { led_on_ = solid; digitalWrite(PIN_ESP_LED, solid ? HIGH : LOW); }
    return;
  }
  if (pat != led_pat_) {          // pattern changed: restart it on an "on" step
    led_pat_ = pat; led_pat_len_ = len; led_pat_idx_ = 0;
    led_last_ms_ = now; led_on_ = true;
    digitalWrite(PIN_ESP_LED, HIGH);
    return;
  }
  if (now - led_last_ms_ >= led_pat_[led_pat_idx_]) {
    led_last_ms_ = now;
    led_pat_idx_ = (uint8_t)((led_pat_idx_ + 1) % led_pat_len_);
    led_on_ = (led_pat_idx_ % 2) == 0;   // even steps are "on"
    digitalWrite(PIN_ESP_LED, led_on_ ? HIGH : LOW);
  }
}

// BOOT button (GPIO0) held for BOOT_FACTORY_MS → wipe NVS to factory defaults
// (open AP, no password) and reboot. The EN button is a hardware reset and
// cannot be sampled, so BOOT serves as the long-press factory-reset control.
void PortController::check_boot_button(uint32_t now) {
  bool pressed = digitalRead(PIN_BOOT_BTN) == LOW;

  // GPIO0 doubles as a strapping pin tied to the USB-serial auto-reset (DTR)
  // circuit. With the board on the programming port but no serial host holding
  // DTR, it can float low at boot and mimic a held BOOT press — which would
  // factory-reset + reboot every BOOT_FACTORY_MS. Ignore any press until the
  // pin has been seen released (high) at least once.
  if (!boot_ready_) {
    if (!pressed) boot_ready_ = true;
    boot_press_ms_ = 0;
    return;
  }

  // Armed: waiting for the confirmation press. A press that lasted the whole
  // window is a stuck/DTR-driven pin, not a person — disarm.
  if (boot_armed_) {
    if (now - boot_armed_ms_ > BOOT_FACTORY_MS + BOOT_CONFIRM_MS) {
      DEBUG_PWR("BOOT: no confirmation press — factory reset cancelled");
      boot_armed_ = false;
      boot_press_ms_ = 0;
      return;
    }
    if (!pressed) {
      // Released. If this ends a SHORT press (a tap), that's the confirmation.
      // A serial monitor's DTR line holds GPIO0 low for as long as the port
      // is open — seconds to minutes — so it can never produce a tap, even
      // across open/close/reopen cycles (which the old edge rule mistook for
      // a confirm and wiped NVS).
      if (boot_press_ms_ != 0 && boot_press_ms_ != boot_armed_ms_) {
        uint32_t held = now - boot_press_ms_;
        if (held >= BOOT_CONFIRM_TAP_MIN_MS && held <= BOOT_CONFIRM_TAP_MAX_MS) {
          DEBUG_PWR("BOOT confirmed (tap %lums) — factory reset", (unsigned long)held);
          boot_armed_ = false;
          wifiFactoryReset();
          requestReboot();
        } else if (held > BOOT_CONFIRM_TAP_MAX_MS) {
          DEBUG_PWR("BOOT: confirmation press too long (%lums) — cancelled", (unsigned long)held);
          boot_armed_ = false;
        }
      }
      boot_press_ms_ = 0;
      return;
    }
    if (boot_press_ms_ == 0) boot_press_ms_ = now;   // confirmation press started
    return;
  }

  if (!pressed) {
    boot_press_ms_ = 0;
    return;
  }
  if (boot_press_ms_ == 0) boot_press_ms_ = now;
  if ((now - boot_press_ms_) >= BOOT_FACTORY_MS) {
    DEBUG_PWR("BOOT held %lums — release and press again within %lus to factory reset",
              (unsigned long)BOOT_FACTORY_MS, (unsigned long)(BOOT_CONFIRM_MS / 1000));
    boot_armed_ = true;
    boot_armed_ms_ = now;
    boot_press_ms_ = now;   // == boot_armed_ms_ marks "still the arming press"
    // The arming press itself must be released before a tap can confirm.
  }
}

// --- external API ---------------------------------------------------------

Status PortController::status() {
  Status st{};
  st.usbc_present = usbc_prev_;
  st.vbus_raw = vbus_raw_;
  st.ecu_powered = ecu_powered_;
  st.ecu_fault = fault_;
  st.ecu_fault_count = fault_count_;
  st.ecu_fault_ago_ms = fault_count_ ? millis() - last_fault_ms_ : UINT32_MAX;
  st.wifi_on = wifiBridgeActive();
  st.wifi_in_use = wifi_in_use_;
  st.asleep = asleep_;
  st.mux = mux_;
  st.route = route_mode_;
  if (asleep_ || usbc_prev_ || route_mode_ != RouteMode::AUTO) {
    st.idle_remaining_ms = 0;
  } else if (!cfg_.idle_auto_off) {
    st.idle_remaining_ms = 0;   // held powered — no countdown
  } else {
    uint32_t el = millis() - last_activity_ms_;
    st.idle_remaining_ms = el >= IDLE_SHUTDOWN_MS ? 0 : (IDLE_SHUTDOWN_MS - el);
  }
  return st;
}

void PortController::set_settings(const Settings &s) {
  cfg_ = s;
  s_prefs.putBool("cDisWifi", cfg_.usbc_disables_wifi);
  s_prefs.putBool("cResWifi", cfg_.restore_wifi_on_unplug);
  s_prefs.putBool("idleOff", cfg_.idle_auto_off);
  DEBUG_PWR("settings: usbcDisablesWifi=%d restoreWifiOnUnplug=%d idleAutoOff=%d",
            cfg_.usbc_disables_wifi, cfg_.restore_wifi_on_unplug, cfg_.idle_auto_off);
  // No wake here: the settings POST came from a web client, which loop()
  // already counts as a request.
}

void PortController::wake() {
  DEBUG_PWR("manual wake — power ECU → ESP host + restart idle window");
  enter_active_esp(millis());
  wifi(true);
}

// Choosing a route in the UI is an explicit request, so it applies now.
void PortController::set_route_override(RouteMode m) {
  route_mode_ = m;
  s_prefs.putUChar("route", (uint8_t)m);
  DEBUG_PWR("route override = %d", (int)m);
  // Re-sync to VBUS immediately so loop() doesn't see a stale edge.
  bool usbc = read_vbus();
  vbus_filter_ = usbc ? 4 : 0;
  usbc_prev_ = usbc;
  enter_active(millis(), usbc);
  if (m == RouteMode::AUTO) wifi(true);
}

}  // namespace port
