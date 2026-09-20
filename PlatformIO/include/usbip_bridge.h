#pragma once

// IgnitronUSB: USB/IP-over-WiFi bridge core.
// Ported from tiehfood/esphome-usbip-printer (GPL-3.0) — ESPHome/printer
// specifics removed, generic vendor-USB device support retained. See LICENSE.
//
// Device-agnostic: every class the ESP32-S3 full/low-speed host can enumerate
// (mass storage, HID, printers, CDC/FTDI serial, vendor bulk ECUs, composite
// devices with alternate settings) is exported as-is. The server intercepts
// only the four requests the Linux usbip stub driver also intercepts —
// SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE(ENDPOINT_HALT) and the
// SET_FEATURE(PORT_RESET) a client uses to mean "reset this device" — because
// each of them resets data toggles on the device and ESP-IDF can only match
// that host-side by destroying and recreating the pipes (interface re-claim).

#include <cstdint>
#include <vector>
#include <functional>
#include <esp_err.h>

namespace usbip {

struct PendingUrb;  // pending-URB table entry (private to usbip_bridge.cpp)

// USB/IP Protocol version
static constexpr uint16_t USBIP_VERSION = 0x0111;

// USB/IP operation codes
enum USBIPOpCode : uint16_t {
  OP_REQ_DEVLIST = 0x8005,
  OP_REP_DEVLIST = 0x0005,
  OP_REQ_IMPORT = 0x8003,
  OP_REP_IMPORT = 0x0003,
  USBIP_CMD_SUBMIT = 0x0001,
  USBIP_RET_SUBMIT = 0x0003,
  USBIP_CMD_UNLINK = 0x0002,
  USBIP_RET_UNLINK = 0x0004,
};

// USB/IP status codes
enum USBIPStatus : uint32_t {
  ST_OK = 0x00000000,
  ST_NA = 0x00000001,
};

// USB/IP header
struct __attribute__((packed)) USBIPHeader {
  uint16_t version;
  uint16_t command;
  uint32_t status;
};

// USB device path info for DEVLIST
struct __attribute__((packed)) USBIPDeviceInfo {
  char path[256];
  char busid[32];
  uint32_t busnum;
  uint32_t devnum;
  uint32_t speed;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bConfigurationValue;
  uint8_t bNumConfigurations;
  uint8_t bNumInterfaces;
};

// USB interface info
struct __attribute__((packed)) USBIPInterfaceInfo {
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t padding;
};

// USBIP_RET_SUBMIT header
struct __attribute__((packed)) USBIPRetSubmit {
  uint32_t command;
  uint32_t seqnum;
  uint32_t devid;
  uint32_t direction;
  uint32_t ep;
  uint32_t status;
  uint32_t actual_length;
  uint32_t start_frame;
  uint32_t number_of_packets;
  uint32_t error_count;
  uint8_t setup[8];
};

// USBIP_RET_UNLINK header
struct __attribute__((packed)) USBIPRetUnlink {
  uint32_t command;
  uint32_t seqnum;
  uint32_t devid;
  uint32_t direction;
  uint32_t ep;
  uint32_t status;
  uint8_t padding[24];
};

// USB device speed (matching Linux USB/IP values)
enum USBIPSpeed : uint32_t {
  USBIP_SPEED_LOW = 1,
  USBIP_SPEED_FULL = 2,
  USBIP_SPEED_HIGH = 3,
};

// Client connection state
enum ClientState {
  CLIENT_STATE_IDLE,
  CLIENT_STATE_DEVLIST,
  CLIENT_STATE_ATTACHED,
};

// Endpoint info (from an endpoint descriptor)
struct EndpointInfo {
  uint8_t address{0};          // full bEndpointAddress incl. 0x80 for IN
  uint8_t attributes{0};       // bmAttributes; bits 1:0 = transfer type
  uint16_t max_packet_size{64};
  uint8_t interval{0};
};

// One interface descriptor (a specific alternate setting) and its endpoints.
struct InterfaceInfo {
  uint8_t interface_number{0};
  uint8_t alt_setting{0};
  uint8_t interface_class{0};
  uint8_t interface_subclass{0};
  uint8_t interface_protocol{0};
  std::vector<EndpointInfo> endpoints;
};

// Stored USB device information
struct USBDevice {
  bool connected{false};
  bool enumerated{false};
  uint8_t dev_addr{0};
  uint16_t vid{0};
  uint16_t pid{0};
  uint16_t bcd_device{0};
  uint8_t device_class{0};
  uint8_t device_subclass{0};
  uint8_t device_protocol{0};
  uint8_t max_packet_size0{8};
  uint8_t num_configurations{1};
  uint8_t current_configuration{0};
  bool configured{false};      // SET_CONFIGURATION(0) from the client clears this
  USBIPSpeed speed{USBIP_SPEED_FULL};

  // Every interface descriptor in the active configuration, all alternate
  // settings included, in descriptor order.
  std::vector<InterfaceInfo> interfaces;

  // Unique interface numbers (descriptor order), the alternate setting the
  // host currently has selected for each, and whether its pipes are claimed.
  std::vector<uint8_t> iface_numbers;
  std::vector<uint8_t> iface_alt;
  std::vector<bool> iface_claimed;

  // Device descriptors (cached)
  std::vector<uint8_t> device_descriptor;
  std::vector<uint8_t> config_descriptor;

  void clear_config() {
    interfaces.clear();
    iface_numbers.clear();
    iface_alt.clear();
    iface_claimed.clear();
    config_descriptor.clear();
    configured = false;
  }
  // Index into iface_numbers/iface_alt/iface_claimed, or -1.
  int iface_index(uint8_t number) const {
    for (size_t i = 0; i < iface_numbers.size(); i++)
      if (iface_numbers[i] == number) return (int)i;
    return -1;
  }
  const InterfaceInfo *find_interface(uint8_t number, uint8_t alt) const {
    for (auto &it : interfaces)
      if (it.interface_number == number && it.alt_setting == alt) return &it;
    return nullptr;
  }
  // The alternate setting currently selected for an interface number.
  const InterfaceInfo *active_interface(uint8_t number) const {
    int idx = iface_index(number);
    if (idx < 0) return nullptr;
    return find_interface(number, iface_alt[idx]);
  }
  uint8_t num_alt_settings(uint8_t number) const {
    uint8_t n = 0;
    for (auto &it : interfaces)
      if (it.interface_number == number) n++;
    return n;
  }
  // Endpoint lookup: the active alternate settings first, then any.
  const EndpointInfo *find_endpoint(uint8_t address) const {
    for (size_t i = 0; i < iface_numbers.size(); i++) {
      const InterfaceInfo *it = find_interface(iface_numbers[i], iface_alt[i]);
      if (!it) continue;
      for (auto &ep : it->endpoints)
        if (ep.address == address) return &ep;
    }
    for (auto &it : interfaces)
      for (auto &ep : it.endpoints)
        if (ep.address == address) return &ep;
    return nullptr;
  }
  // Interface number owning an endpoint (active alt settings), or -1.
  int interface_of_endpoint(uint8_t address) const {
    for (size_t i = 0; i < iface_numbers.size(); i++) {
      const InterfaceInfo *it = find_interface(iface_numbers[i], iface_alt[i]);
      if (!it) continue;
      for (auto &ep : it->endpoints)
        if (ep.address == address) return iface_numbers[i];
    }
    return -1;
  }
};

class USBIPComponent {
 public:
  USBIPComponent();
  ~USBIPComponent();

  void setup();
  void loop();
  void dump_config();

  void set_port(uint16_t port) { this->port_ = port; }

  // True while a USB/IP client is attached (used by the idle watchdog).
  bool client_attached() const { return this->client_state_ == CLIENT_STATE_ATTACHED; }

  // True if any URB was submitted or answered within the last window_ms
  // (status LED "transferring" state).
  bool active_within(uint32_t window_ms) const;

  // Snapshot of the attached USB device, for the diagnostics UI.
  struct DeviceInfo {
    bool present;         // a device is electrically connected
    bool ready;           // enumeration finished (descriptors read)
    uint16_t vid;
    uint16_t pid;
    uint8_t usb_class;    // effective class: device descriptor, else 1st interface
  };
  // Plain-struct snapshot, refreshed by the bridge task after enumeration /
  // disconnect, so other tasks (web UI, LED) never touch the descriptor
  // vectors while they are being rebuilt.
  DeviceInfo device_info() const { return this->dev_info_cache_; }

  // USB Host callbacks
  void on_device_connected(uint8_t dev_addr);
  void on_device_disconnected(uint8_t dev_addr);

 protected:
  // Network handling
  void start_server_();
  void handle_client_();
  void disconnect_client_();

  // Protocol handling
  bool handle_devlist_request_();
  bool handle_import_request_();
  bool handle_urb_();

  // Pending-URB engine (ported from the reference USB/IP servers): one
  // usb_transfer_t per CMD_SUBMIT, submitted immediately; completions are
  // polled from loop() and answered in completion order.
  //   stream_out: the OUT payload is still in the TCP socket and is pulled
  //   into the USB transfer chunk by chunk (large OUT URBs).
  bool handle_cmd_submit_(uint32_t seqnum, uint32_t ep, uint32_t direction,
                          uint32_t transfer_flags, uint32_t length,
                          const uint8_t *setup, const uint8_t *out_data,
                          bool stream_out);
  void poll_pending_urbs_();
  esp_err_t submit_urb_chunk_(PendingUrb *p);

  // Standard requests the server must run itself (same set the Linux usbip
  // stub driver intercepts) because they reset data toggles on the device.
  void handle_set_configuration_(uint32_t seqnum, const uint8_t *setup);
  void handle_set_interface_(uint32_t seqnum, const uint8_t *setup);
  void handle_clear_halt_(uint32_t seqnum, const uint8_t *setup);
  void handle_reset_device_(uint32_t seqnum, const uint8_t *setup);
  // Shared body of SET_CONFIGURATION and the device-reset emulation: kill
  // in-flight URBs, tear down every pipe, send SET_CONFIGURATION(value) to the
  // device, re-claim. Returns the device's answer to the request.
  esp_err_t reconfigure_device_(uint8_t value);

  // Synchronous no-data control request (used by the intercepts above).
  esp_err_t sync_control_out_(const uint8_t *setup);

  // Modem-line (DTR/RTS) coalescing for USB-serial adapters: see the comment
  // block in usbip_bridge.cpp. Returns true if the request was absorbed.
  bool absorb_line_state_(uint32_t seqnum, const uint8_t *setup);
  void flush_line_state_(bool force);

  // Pipe lifecycle. ESP-IDF exposes no data-toggle reset (hcd_dwc.c: only
  // pipe creation starts at DATA0), so recreating an interface's pipes via
  // release + re-claim is the host-side equivalent of usb_reset_endpoint().
  //   quiesce_interface_: cancel every in-flight URB on the interface's active
  //     endpoints so the pipes can be freed. kill=false → the URBs are marked
  //     for transparent resubmission on the new pipes (CLEAR_FEATURE);
  //     kill=true → they are failed with -ESHUTDOWN (config/alt change).
  void quiesce_interface_(const InterfaceInfo &iface, bool kill);
  void quiesce_all_(bool kill);
  bool claim_interface_(uint8_t number, uint8_t alt);
  bool release_interface_(uint8_t number);
  void claim_all_interfaces_();
  void release_all_interfaces_();
  bool recreate_interface_pipes_(uint8_t number);

  // Max packet size of an endpoint from its descriptor (ep0: bMaxPacketSize0).
  uint16_t mps_for_ep_(uint8_t ep_addr) const;

  // USB/IP message helpers
  void send_devlist_response_();
  void send_import_response_(bool success);
  void send_urb_response_(uint32_t seqnum, int32_t status, const uint8_t *data, uint32_t length);

  // USB host handling
  void usb_host_init_();
  bool enumerate_device_(uint8_t dev_addr);
  bool parse_config_descriptor_();

  // Utility functions
  static uint16_t htons_(uint16_t value);
  static uint32_t htonl_(uint32_t value);
  static uint16_t ntohs_(uint16_t value);
  static uint32_t ntohl_(uint32_t value);

  bool read_bytes_(uint8_t *buffer, size_t length, uint32_t timeout_ms = 1000);
  bool write_bytes_(const uint8_t *buffer, size_t length);

  // Configuration
  uint16_t port_{3240};

  // Server state (using BSD sockets)
  int server_fd_{-1};
  int client_fd_{-1};
  ClientState client_state_{CLIENT_STATE_IDLE};
  // Protocol version echoed back to the client. Captured from the OP_REQ header
  // so we speak whatever the attaching tool uses (Linux usbip / usbip-win2 both
  // send 0x0111, older clients 0x0100) instead of forcing our compiled default.
  uint16_t negotiated_version_{USBIP_VERSION};

  // USB device state
  USBDevice usb_device_;
  DeviceInfo dev_info_cache_{};
  void refresh_device_info_();
  bool usb_host_initialized_{false};
  void *usb_host_client_hdl_{nullptr};

  // Timing
  uint32_t last_usb_check_{0};
  volatile uint32_t last_urb_ms_{0};   // millis() of the last URB submit/reply
};

// Global component reference for callbacks
extern USBIPComponent *g_usbip_component;

// One-shot replay of the ECU session-start handshake captured off a real
// Ignitron session (tools/tapcap.py + the ep0 control-transfer tap in
// ecu_tap.cpp — every value was identical across repeats, i.e. a plain
// replay, no session nonce to derive). Lets the ECU start talking without a
// laptop/usbip client attached. Returns false if no ECU is enumerated, a
// usbip client is already attached (refuses rather than risk colliding with
// real control traffic), or any step fails/stalls.
bool replay_ecu_handshake();
// Why the last replay_ecu_handshake() failed (empty string after a success):
// "no ECU", "USB/IP client attached", or "step N bReq=.. wValue=.. wIndex=..: <esp err>".
const char *last_launch_error();

// Continuous standalone gauge poll (see usbip_bridge.cpp for the "why" — the
// handshake above never asks for gauge data on its own). Starting it fires a
// one-time bulk "poke" then loops bulk-IN reads on ep 0x81 into the existing
// decoder for as long as it's on; it steps aside automatically whenever a
// real usbip client is attached. Safe to call repeatedly with the same value.
void set_gauge_poll(bool on);

// --- ECU fault memory (Ignitron "Fault codes" window) ------------------------
// Layout, from Ignitron.exe's parser and a captured read/clear (faults1.bin):
// the DSP table is 512 B at bank 2 word 0x200, the CPU table 1024 B at bank 3
// word 0xC00; both are 32-byte pages of three 5-word records + a checksum word
// (u16 sum of the page's other 30 bytes). A record is
//   [code, rpm, load x0.1 %, value, (count-1)<<12 | duration]
// and code 0xFFFF is an empty slot. The word just before each table
// (0x1F8 / 0xBF8) carries a u16 counter at bytes 6..7 that the PC reads along.
struct EcuFault {
  uint16_t code;      // Ignitron P-code (names in data/faults.js)
  uint16_t rpm;
  uint16_t load;      // x0.1 %
  uint16_t value;     // fault-specific; 9999 / 32767 = n/a
  uint8_t count;      // occurrences, 1..16
  uint16_t duration;  // low 12 bits of the last word
  uint8_t src;        // 1 = DSP, 2 = CPU
  bool page_ok;       // the page's checksum matched
};
struct EcuFaultMemory {
  EcuFault faults[48 + 96];
  size_t count;
  uint16_t dsp_counter, cpu_counter;
  bool dsp_ok, cpu_ok;       // each table read completely
  bool dsp_csum, cpu_csum;   // every non-empty page's checksum matched
};
// Both take the ECU bus from the gauge poll for the duration (~100 ms / ~1 s).
// They refuse (false, reason in last_launch_error()) when no ECU is
// enumerated or a USB/IP client owns the device.
bool ecu_read_faults(EcuFaultMemory &out);
bool ecu_clear_faults();
bool gauge_poll_active();

}  // namespace usbip
