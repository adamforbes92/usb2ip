#pragma once

// usb2ip — USB routing + ECU 5V power state machine.
//
// Owns the board-control GPIOs (see defs.h) and sequences them through the
// operating stages. The USB-A port is only powered ON REQUEST — never just
// because the board powered up:
//
//   0  Boot/standby   ECU 5V OFF, mux open (Hi-Z), WiFi AP up waiting for a request.
//   1  USB-A / ESP    a WiFi client appears (station joins / web request / USB/IP
//                      attach) → ECU 5V on, mux → ESP host, 5-min timer armed.
//   2  USB-C inserted  VBUS_SENSE high → ECU 5V on, mux → laptop (USB-C),
//                      optionally drop the WiFi bridge.
//   3  USB-C removed   back to standby (optionally restore the WiFi bridge); a
//                      WiFi client still present re-wakes straight into stage 1.
//   4  Idle 5 min     no WiFi activity → cut ECU 5V + open the mux (ECU sees
//                      unplug). The AP stays up so the next client can wake it.
//
// begin()/loop() run in the Arduino main task. status()/settings() are read by
// the async web-server task (scalar snapshot — no lock needed).

#include <cstdint>

namespace port {

enum class MuxMode : uint8_t { OFF, ESP_HOST, LAPTOP };

// Manual routing override. Pins WHERE the ECU (USB-A) goes once a request
// powers it; it does not power the port by itself at boot:
//   AUTO  follow VBUS sense (original behaviour)
//   USBC  force mux -> laptop (USB-C), hold powered once woken
//   WIFI  force mux -> ESP host + WiFi bridge, VBUS ignored, hold powered once woken
// Selecting a route in the UI is itself a request, so it applies immediately.
enum class RouteMode : uint8_t { AUTO, USBC, WIFI };

struct Settings {
  bool usbc_disables_wifi;      // stage 2: turn the WiFi bridge off on USB-C insert
  bool restore_wifi_on_unplug;  // stage 3: turn it back on when USB-C is removed
  bool idle_auto_off;           // stage 4: cut ECU 5V after 5 min idle (off = stay powered)
};

struct Status {
  bool usbc_present;            // debounced VBUS_SENSE
  bool vbus_raw;                // instantaneous VBUS_SENSE pin level (diagnostics)
  bool ecu_powered;             // firmware has the TPS2041B enabled (commanded, not measured)
  bool ecu_fault;               // TPS2041B OC# asserted NOW: over-current / thermal shutdown
  uint32_t ecu_fault_count;     // OC# assertions since boot (edge-counted by ISR, catches short ones)
  uint32_t ecu_fault_ago_ms;    // ms since the last one (UINT32_MAX = none since boot)
  bool wifi_on;
  bool wifi_in_use;             // a station is associated or a USB/IP client is attached
  bool asleep;                  // standby: port unpowered, waiting for a request (boot / stage 4)
  MuxMode mux;
  RouteMode route;              // active manual override (AUTO = follow VBUS)
  uint32_t idle_remaining_ms;   // countdown to auto power-down (0 = not counting)
};

class PortController {
 public:
  void begin();
  void loop();

  Status status();
  Settings settings() const { return cfg_; }
  void set_settings(const Settings &s);   // applies + persists to NVS
  void wake();                            // power ECU → ESP host + restart the idle window
  void set_route_override(RouteMode m);   // pin ECU routing (persists to NVS)

 private:
  void ecu_power(bool on);
  void mux(MuxMode mode);
  void wifi(bool on);
  void enter_active(uint32_t now, bool usbc);
  void enter_active_esp(uint32_t now);
  void enter_standby(const char *why);
  bool compute_wifi_in_use();
  bool read_vbus();
  void check_fault();
  void update_led(uint32_t now);
  void check_boot_button(uint32_t now);

  Settings cfg_{};
  MuxMode mux_{MuxMode::OFF};
  RouteMode route_mode_{RouteMode::AUTO};
  bool ecu_powered_{false};
  bool asleep_{false};
  bool usbc_prev_{false};
  bool vbus_raw_{false};
  bool wifi_in_use_{false};
  bool fault_{false};           // last OC# state seen (edge-logged)
  uint32_t fault_count_{0};     // OC# falling edges consumed from the ISR
  uint32_t last_fault_ms_{0};   // millis() of the most recent one
  uint8_t vbus_filter_{0};
  uint32_t last_activity_ms_{0};
  uint32_t last_poll_ms_{0};
  uint32_t led_last_ms_{0};
  bool led_on_{false};
  const uint16_t *led_pat_{nullptr};   // current on/off pattern (ms pairs), nullptr = static
  uint8_t led_pat_len_{0};
  uint8_t led_pat_idx_{0};
  uint32_t boot_press_ms_{0};   // millis() when BOOT was first seen low (0 = up)
  bool boot_ready_{false};      // BOOT seen released once (guards GPIO0 low-at-boot)
  // Factory-reset confirmation: hold >= BOOT_FACTORY_MS arms it (LED strobes);
  // it only fires on a *release and re-press* within BOOT_CONFIRM_MS. A DTR
  // line holding GPIO0 low can never produce that edge pattern.
  bool boot_armed_{false};
  uint32_t boot_armed_ms_{0};   // millis() when armed (release deadline base)
};

extern PortController g_port;

}  // namespace port
