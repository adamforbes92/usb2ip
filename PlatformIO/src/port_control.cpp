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

// --- low-level pin actions ------------------------------------------------

void PortController::ecu_power(bool on) {
  if (on) {
    pinMode(PIN_IGNITRON_PWR, OUTPUT);
    digitalWrite(PIN_IGNITRON_PWR, LOW);   // open-drain LOW → PMOS fully on
  } else {
    pinMode(PIN_IGNITRON_PWR, INPUT);      // Hi-Z → gate floats to +5V → off
  }
  ecu_powered_ = on;
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

// --- stage transitions ----------------------------------------------------

void PortController::enter_active_esp(uint32_t now) {
  asleep_ = false;
  ecu_power(true);
  mux(MuxMode::ESP_HOST);
  last_activity_ms_ = now;   // arm / restart the 5-minute idle window
}

// Pin the ECU to a fixed destination, ignoring VBUS sense.
void PortController::apply_forced_route(uint32_t now) {
  asleep_ = false;
  ecu_power(true);
  if (route_mode_ == RouteMode::USBC) {
    mux(MuxMode::LAPTOP);
    if (cfg_.usbc_disables_wifi) wifi(false);
  } else {  // RouteMode::WIFI
    mux(MuxMode::ESP_HOST);
    wifi(true);
  }
  last_activity_ms_ = now;
}

void PortController::enter_sleep() {
  DEBUG_PWR("idle %lus — WiFi bridge + ECU 5V off (ECU sees unplug)",
            (unsigned long)(IDLE_SHUTDOWN_MS / 1000));
  wifi(false);
  ecu_power(false);
  mux(MuxMode::OFF);
  asleep_ = true;
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
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  pinMode(PIN_USB_OE, OUTPUT);
  digitalWrite(PIN_USB_OE, HIGH);            // start disconnected
  pinMode(PIN_USB_SEL, OUTPUT);
  digitalWrite(PIN_USB_SEL, MUX_SEL_ESP);

  bool usbc = digitalRead(PIN_VBUS_SENSE) == HIGH;
  usbc_prev_ = usbc;
  vbus_filter_ = usbc ? 4 : 0;

  uint32_t now = millis();
  if (route_mode_ != RouteMode::AUTO) {
    // Manual override pinned in NVS — enforce it, ignore VBUS.
    apply_forced_route(now);
  } else if (usbc) {
    // Booted with a laptop already on USB-C → hand the ECU straight to it.
    asleep_ = false;
    ecu_power(true);
    mux(MuxMode::LAPTOP);
    if (cfg_.usbc_disables_wifi) wifi(false);
    last_activity_ms_ = now;
  } else {
    // Normal boot: ESP hosts the ECU, WiFi bridge already up (from main).
    enter_active_esp(now);
  }

  last_poll_ms_ = now;
  led_last_ms_ = now;
  DEBUG_PWR("port control up — usbc=%d, ecu=%d, mux=%d, route=%d",
            usbc, ecu_powered_, (int)mux_, (int)route_mode_);
}

void PortController::loop() {
  uint32_t now = millis();
  update_led(now);

  if (now - last_poll_ms_ < 50) return;     // poll VBUS / timer at 20 Hz
  last_poll_ms_ = now;

  check_boot_button(now);

  // Manual override: routing is pinned, VBUS auto-switch + idle sleep disabled.
  if (route_mode_ != RouteMode::AUTO) {
    wifi_in_use_ = compute_wifi_in_use();
    last_activity_ms_ = now;
    return;
  }

  // Debounced USB-C presence (needs 4 consistent samples to flip either way).
  bool raw = digitalRead(PIN_VBUS_SENSE) == HIGH;
  if (raw && vbus_filter_ < 4) vbus_filter_++;
  if (!raw && vbus_filter_ > 0) vbus_filter_--;
  bool usbc = usbc_prev_;
  if (vbus_filter_ >= 4) usbc = true;
  if (vbus_filter_ == 0) usbc = false;

  if (usbc && !usbc_prev_) {
    // Stage 2 — USB-C inserted.
    DEBUG_PWR("USB-C connected → mux to laptop%s",
              cfg_.usbc_disables_wifi ? ", WiFi bridge off" : "");
    asleep_ = false;
    ecu_power(true);
    mux(MuxMode::LAPTOP);
    if (cfg_.usbc_disables_wifi) wifi(false);
  } else if (!usbc && usbc_prev_) {
    // Stage 3 — USB-C removed.
    DEBUG_PWR("USB-C removed → mux to ESP host%s",
              cfg_.restore_wifi_on_unplug ? ", WiFi bridge restored" : "");
    enter_active_esp(now);
    if (cfg_.restore_wifi_on_unplug) wifi(true);
  }
  usbc_prev_ = usbc;

  wifi_in_use_ = compute_wifi_in_use();

  if (usbc) {                 // laptop present → never auto power-down
    last_activity_ms_ = now;
    return;
  }
  if (asleep_) return;        // already down; USB-C insertion is the wake source

  // Stage 4 — idle watchdog. Disabled → hold the ECU powered indefinitely.
  if (!cfg_.idle_auto_off) {
    last_activity_ms_ = now;
    return;
  }
  if (wifi_in_use_) last_activity_ms_ = now;
  if (now - last_activity_ms_ >= IDLE_SHUTDOWN_MS) enter_sleep();
}

// --- status LED -----------------------------------------------------------
// Patterns are (on, off) millisecond pairs, looped. See defs.h for meanings.
static const uint16_t kLedSlow[]   = {150, 1350};                 // no device
static const uint16_t kLedDouble[] = {120, 150, 120, 1200};       // device idle
static const uint16_t kLedFast[]   = {80, 80};                    // transferring
static const uint16_t kLedStrobe[] = {40, 40};                    // reset armed

void PortController::update_led(uint32_t now) {
  const uint16_t *pat = nullptr;
  uint8_t len = 0;
  bool solid = false, off = false;

  bool dev_present = usbip::g_usbip_component && usbip::g_usbip_component->device_info().present;
  bool xfer = usbip::g_usbip_component && usbip::g_usbip_component->active_within(LED_ACTIVE_MS);
  bool laptop = (mux_ == MuxMode::LAPTOP);

  if (boot_armed_)      { pat = kLedStrobe; len = 2; }
  else if (asleep_)     { off = true; }
  else if (laptop)      { solid = true; }
  else if (xfer)        { pat = kLedFast; len = 2; }
  else if (dev_present) { pat = kLedDouble; len = 4; }
  else                  { pat = kLedSlow; len = 2; }

  if (off || solid) {
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
  st.ecu_powered = ecu_powered_;
  st.wifi_on = wifiBridgeActive();
  st.wifi_in_use = wifi_in_use_;
  st.asleep = asleep_;
  st.mux = mux_;
  st.route = route_mode_;
  if (asleep_ || usbc_prev_) {
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
  // Applying "stay powered" mid-idle should wake immediately if already asleep.
  if (!cfg_.idle_auto_off && asleep_ && route_mode_ == RouteMode::AUTO)
    enter_active_esp(millis());
}

void PortController::wake() {
  DEBUG_PWR("manual wake — re-power ECU + restart idle window");
  enter_active_esp(millis());
  wifi(true);
}

void PortController::set_route_override(RouteMode m) {
  route_mode_ = m;
  s_prefs.putUChar("route", (uint8_t)m);
  DEBUG_PWR("route override = %d", (int)m);
  uint32_t now = millis();
  if (m == RouteMode::AUTO) {
    // Re-sync to VBUS immediately, then let loop() resume auto switching.
    bool usbc = digitalRead(PIN_VBUS_SENSE) == HIGH;
    vbus_filter_ = usbc ? 4 : 0;
    usbc_prev_ = usbc;
    asleep_ = false;
    ecu_power(true);
    mux(usbc ? MuxMode::LAPTOP : MuxMode::ESP_HOST);
    wifi(true);
    last_activity_ms_ = now;
  } else {
    apply_forced_route(now);
  }
}

}  // namespace port
