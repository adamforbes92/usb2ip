#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// IgnitronUSB — build/version + WiFi soft-AP config
// ---------------------------------------------------------------------------
#define FW_VERSION "0.5"

// Soft-AP the laptop connects to before running the usbip-win2 client. These
// are the factory DEFAULTS only — the live SSID/password are stored in NVS and
// editable from the web UI (Settings tab). Default password is empty = OPEN AP.
// A WPA2 password, if set, must be >= 8 characters.
#define AP_SSID     "IgnitronUSB"
#define AP_PASSWORD ""            // empty = open network (no password)

// Soft-AP 2.4 GHz channel — factory DEFAULT only; the live value is stored in
// NVS and chosen from the web UI (Settings → WiFi channel, with a band scan).
// 1/6/11 are the non-overlapping choices. The bridge needs ~5 Mbit/s, so a
// clean channel matters far more than a fast one: every lost WiFi frame costs
// a lwIP retransmission-timer stall (0.5-1 s) on the USB/IP stream.
#define AP_CHANNEL 6

// Regulatory domain for the soft-AP. The WiFi driver boots in "world-safe"
// mode (cc=01, channels 1-11 only); an ETSI country unlocks 12/13. The saved
// channel is clamped to AP_COUNTRY_NCHAN, and a channel the driver still
// rejects falls back to AP_CHANNEL rather than leaving the AP down.
#define AP_COUNTRY        "GB"
#define AP_COUNTRY_NCHAN  13

// Soft-AP network address — the web UI and USB/IP host live here (192.168.1.1).
#define AP_IP_0 192
#define AP_IP_1 168
#define AP_IP_2 1
#define AP_IP_3 1

// Standard USB/IP TCP port (Linux + usbip-win2 default).
#define USBIP_TCP_PORT 3240

// Raw USB-frame tap: a second TCP port that streams length-prefixed records of
// every ECU bulk transfer, for capturing to a file (Wireshark/netcat) while
// reverse-engineering the gauge protocol. 0 disables the tap server.
#define USBIP_TAP_PORT 3241

// ---------------------------------------------------------------------------
// ECU telemetry (gauge) capture
//
//   * ECU_DATA_EP  — bulk endpoint number the gauge blocks arrive on. The ECU
//     answers the 0xFA poll on bulk-IN endpoint 1 (8 B header, then the 64 B
//     or 704 B channel block); 0 = feed the decoder from ANY IN endpoint.
//   * ECU_POLL_MODE — how gauge frames are obtained:
//       0 = piggyback only (decode while the Ignitron PC polls)
//       1 = active only    (ESP32 polls the ECU itself, no PC needed)
//       2 = auto           (piggyback when a USB/IP client is attached,
//                           otherwise active)  ** recommended **
//   * ECU_MOCK — synthesise plausible engine values so the web gauges can be
//     developed without an ECU on the bench. 0 = off for real use.
// ---------------------------------------------------------------------------
#define ECU_DATA_EP   1        // bulk-IN ep1 (see session1.bin); 0 = any
#define ECU_POLL_MODE 2        // 0 piggyback, 1 active, 2 auto
#define ECU_MOCK      0        // 1 = feed the UI with synthetic telemetry

// Web gauge UI (LittleFS SPA polling /api/status).
#define WEB_HTTP_PORT 80

// ESP32-S3 native USB OTG PHY data pins — the USB-A (ECU) socket breaks out
// here, through the FSUSB42 mux. Fixed in silicon; listed for wiring only.
#define PIN_USB_DMINUS 19   // D-  (ESP_IGNITRON_D-)
#define PIN_USB_DPLUS  20   // D+  (ESP_IGNITRON_D+)

// ---------------------------------------------------------------------------
// Board I/O — USB routing + ECU 5V power control (schematic V1.0)
//
//   GPIO4  ESP_LED       status LED (active-HIGH: pin HIGH = lit)
//   GPIO5  USB_OE        FSUSB42 /OE  — HIGH = mux Hi-Z (USB-A floating/off)
//   GPIO6  USB_SEL       FSUSB42 SEL  — LOW = ESP host, HIGH = laptop (USB-C)
//   GPIO7  IGNITRON_PWR  ECU 5V PMOS gate — OPEN-DRAIN ACTIVE-LOW (see below)
//   GPIO8  VBUS_SENSE    USB-C VBUS present (Schmitt-buffered) — HIGH = C plugged
//
// IGNITRON_PWR drives an AO3401A PMOS gate that has a 100k pull-up to +5V:
//   * Enable  ECU 5V : pinMode(OUTPUT) + digitalWrite(LOW) → gate low, PMOS on.
//   * Disable ECU 5V : pinMode(INPUT)  (Hi-Z)              → gate floats to +5V, off.
//   * NEVER drive it HIGH (3.3V) — that only partially turns the PMOS on and
//     makes it dissipate heat. Hi-Z (INPUT) is the safe "off".
// ---------------------------------------------------------------------------
#define PIN_ESP_LED       4
#define PIN_USB_OE        5
#define PIN_USB_SEL       6
#define PIN_IGNITRON_PWR  7
#define PIN_VBUS_SENSE    8

// BOOT button (SW_BOOT → GPIO0, strapping pin, external pull-up). Hold it for
// BOOT_FACTORY_MS (LED strobes = armed), release, then press again within
// BOOT_CONFIRM_MS (a short tap, under 1.5 s) to clear NVS back to factory
// defaults — the open AP with no password — and reboot. The confirmation press
// exists because GPIO0 is also driven by the CH340's DTR auto-reset circuit: a
// serial terminal asserting DTR holds it low indefinitely and, on a plain
// long-press rule, silently factory-reset the board (the routing override
// "randomly" reverting to AUTO). NOTE: the EN button is a hardware reset
// (holds CHIP_PU low) and cannot be sampled in firmware.
#define PIN_BOOT_BTN      0
#define BOOT_FACTORY_MS   3000
#define BOOT_CONFIRM_MS   3000
#define BOOT_CONFIRM_TAP_MIN_MS 60     // confirmation = a TAP, not a hold
#define BOOT_CONFIRM_TAP_MAX_MS 1500   // (a DTR line can't tap)

// FSUSB42 SEL logic levels.
#define MUX_SEL_ESP     LOW    // ECU D± ↔ ESP32 native USB host (WiFi bridge)
#define MUX_SEL_LAPTOP  HIGH   // ECU D± ↔ USB-C (piggyback laptop, direct)

// Auto power-down: after this long with no WiFi activity (no station associated
// and no USB/IP client attached), the WiFi bridge AND the ECU 5V rail both cut,
// so the ECU sees an unplugged host. USB-C insertion overrides / wakes it.
#define IDLE_SHUTDOWN_MS (5UL * 60UL * 1000UL)

// Status LED — what the ESP is doing with the USB-A device:
//   slow flash     no device connected to USB-A
//   double blink   device connected, idle (two short flashes, long gap)
//   fast blink     transferring / in use (URB traffic within LED_ACTIVE_MS)
//   solid          device handed to the laptop over USB-C (ESP not hosting)
//   off            asleep (idle power-down)
//   strobe         BOOT factory reset armed, awaiting the confirmation press
#define LED_ACTIVE_MS 750        // "transferring" hold time after the last URB

// Settings defaults (persisted in NVS, overridable from the web UI).
// NOTE: on this PCB the VBUS-sense net is also fed by the programming/USB-C
// port, so it reads present whenever the board is powered — keep WiFi up.
#define DEFAULT_USBC_DISABLES_WIFI     0  // 0 = keep WiFi bridge up when VBUS present
#define DEFAULT_RESTORE_WIFI_ON_UNPLUG 1  // restore WiFi bridge on USB-C removal
#define DEFAULT_IDLE_AUTO_OFF          1  // 1 = 5-min idle power-down; 0 = stay powered

// ---------------------------------------------------------------------------
// Serial debug (AirLift-style tagged, per-subsystem)
//
//   * enableDebug is the SINGLE control point. Set to 0 to silence everything.
//   * The USB/IP + USB-host core uses ESP-IDF ESP_LOGx (tags "usbip"/"usb_host",
//     level set by CORE_DEBUG_LEVEL); these macros are for the app layer.
// ---------------------------------------------------------------------------
#define enableDebug 1        // ** MASTER ** 0 = silence ALL serial debug

#define debugSys    1        // [SYS]   boot / general / 1 Hz telemetry
#define debugPwr    1        // [PWR]   ECU power, USB routing, and LED state
#define debugWifi   1        // [WiFi]  soft-AP bring-up
#define debugUSB    1        // [USB]   bridge lifecycle
#define debugEcu    1        // [ECU]   gauge decode / active poll
#define debugTap    1        // [TAP]   raw frame tap / capture server
#define debugWeb    1        // [WEB]   HTTP + WebSocket gauge UI

#define serialDebugBaud 115200

// --- Master / general (uncategorised + 1 Hz telemetry) ---
#if enableDebug && debugSys
#define DEBUG(x, ...)  Serial.printf("[SYS] " x "\n", ##__VA_ARGS__)
#define DEBUG_(x, ...) Serial.printf("[SYS] " x, ##__VA_ARGS__)
#else
#define DEBUG(x, ...)
#define DEBUG_(x, ...)
#endif

// --- [PWR] ECU power and USB routing ---
#if enableDebug && debugPwr
#define DEBUG_PWR(x, ...)  Serial.printf("[PWR] " x "\n", ##__VA_ARGS__)
#define DEBUG_PWR_(x, ...) Serial.printf("[PWR] " x, ##__VA_ARGS__)
#else
#define DEBUG_PWR(x, ...)
#define DEBUG_PWR_(x, ...)
#endif

// --- [WiFi] soft-AP ---
#if enableDebug && debugWifi
#define DEBUG_WIFI(x, ...)  Serial.printf("[WiFi] " x "\n", ##__VA_ARGS__)
#define DEBUG_WIFI_(x, ...) Serial.printf("[WiFi] " x, ##__VA_ARGS__)
#else
#define DEBUG_WIFI(x, ...)
#define DEBUG_WIFI_(x, ...)
#endif

// --- [USB] bridge lifecycle ---
#if enableDebug && debugUSB
#define DEBUG_USB(x, ...)  Serial.printf("[USB] " x "\n", ##__VA_ARGS__)
#define DEBUG_USB_(x, ...) Serial.printf("[USB] " x, ##__VA_ARGS__)
#else
#define DEBUG_USB(x, ...)
#define DEBUG_USB_(x, ...)
#endif

// --- [ECU] gauge decode / active poll ---
#if enableDebug && debugEcu
#define DEBUG_ECU(x, ...)  Serial.printf("[ECU] " x "\n", ##__VA_ARGS__)
#define DEBUG_ECU_(x, ...) Serial.printf("[ECU] " x, ##__VA_ARGS__)
#else
#define DEBUG_ECU(x, ...)
#define DEBUG_ECU_(x, ...)
#endif

// --- [TAP] raw frame tap / capture server ---
#if enableDebug && debugTap
#define DEBUG_TAP(x, ...)  Serial.printf("[TAP] " x "\n", ##__VA_ARGS__)
#define DEBUG_TAP_(x, ...) Serial.printf("[TAP] " x, ##__VA_ARGS__)
#else
#define DEBUG_TAP(x, ...)
#define DEBUG_TAP_(x, ...)
#endif

// --- [WEB] HTTP + WebSocket gauge UI ---
#if enableDebug && debugWeb
#define DEBUG_WEB(x, ...)  Serial.printf("[WEB] " x "\n", ##__VA_ARGS__)
#define DEBUG_WEB_(x, ...) Serial.printf("[WEB] " x, ##__VA_ARGS__)
#else
#define DEBUG_WEB(x, ...)
#define DEBUG_WEB_(x, ...)
#endif
