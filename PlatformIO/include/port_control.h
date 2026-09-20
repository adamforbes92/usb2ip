#pragma once

// IgnitronUSB — USB routing + ECU 5V power state machine.
//
// Owns the four board-control GPIOs (see defs.h) and sequences them through the
// four operating stages:
//
//   0  Boot           ECU 5V on, mux → ESP host, WiFi bridge up, 5-min timer armed.
//   1  USB-A / ESP    ESP hosts the ECU and exports it over WiFi (USB/IP).
//   2  USB-C inserted  VBUS_SENSE high → hand the ECU to the laptop (mux → USB-C),
//                      optionally drop the WiFi bridge.
//   3  USB-C removed   restore mux → ESP host, optionally restore the WiFi bridge.
//   4  Idle 5 min     no WiFi activity → drop WiFi + cut ECU 5V (ECU sees unplug).
//
// begin()/loop() run in the Arduino main task. status()/settings() are read by
// the async web-server task (scalar snapshot — no lock needed).

#include <cstdint>

namespace port {

enum class MuxMode : uint8_t { OFF, ESP_HOST, LAPTOP };

// Manual routing override. VBUS auto-detect is unreliable on this PCB (bad
// route), so the UI can pin the ECU (USB-A) to a fixed destination:
//   AUTO  follow VBUS sense (original behaviour)
//   USBC  force mux -> laptop (USB-C), hold powered
//   WIFI  force mux -> ESP host + WiFi bridge, hold powered
enum class RouteMode : uint8_t { AUTO, USBC, WIFI };

struct Settings {
  bool usbc_disables_wifi;      // stage 2: turn the WiFi bridge off on USB-C insert
  bool restore_wifi_on_unplug;  // stage 3: turn it back on when USB-C is removed
  bool idle_auto_off;           // stage 4: cut ECU 5V after 5 min idle (off = stay powered)
};

struct Status {
  bool usbc_present;
  bool ecu_powered;
  bool wifi_on;
  bool wifi_in_use;             // a station is associated or a USB/IP client is attached
  bool asleep;                  // idle power-down latched (stage 4)
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
  void wake();                            // re-power ECU + restart the idle window
  void set_route_override(RouteMode m);   // pin ECU routing (persists to NVS)

 private:
  void ecu_power(bool on);
  void mux(MuxMode mode);
  void wifi(bool on);
  void apply_forced_route(uint32_t now);
  void enter_active_esp(uint32_t now);
  void enter_sleep();
  bool compute_wifi_in_use();
  void update_led(uint32_t now);
  void check_boot_button(uint32_t now);

  Settings cfg_{};
  MuxMode mux_{MuxMode::OFF};
  RouteMode route_mode_{RouteMode::AUTO};
  bool ecu_powered_{false};
  bool asleep_{false};
  bool usbc_prev_{false};
  bool wifi_in_use_{false};
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
