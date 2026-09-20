#include "usbip_bridge.h"

#include <Arduino.h>

#include "defs.h"
#include "ecu_tap.h"
#include "crumb.h"

#include <algorithm>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// ESP-IDF USB Host includes
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_intr_alloc.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

// The USB transfer helpers below feed the task watchdog from several task
// contexts (daemon task, loopTask, TCP handler), none of which subscribe to
// the TWDT. A bare esp_task_wdt_reset() from an unsubscribed task logs
// "esp_task_wdt_reset(): task not found" on every call, flooding the console.
// Route every reset through a guard that only feeds when actually subscribed.
static inline void usbip_feed_wdt() {
  if (esp_task_wdt_status(NULL) == ESP_OK)
    esp_task_wdt_reset();
}
#define esp_task_wdt_reset() usbip_feed_wdt()

// Per-URB tracing. Off by default: at 115200 baud every URB costs ~15 ms of
// blocking UART output, which is more than a serial adapter's whole 16 ms
// latency-timer period — the bridge itself becomes the bottleneck and
// high-rate drivers (FTDI/CDC status polls, HID) time out. Lifecycle events
// (attach, enumeration, errors) are always logged.
#ifndef USBIP_TRACE_URBS
#define USBIP_TRACE_URBS 0
#endif
#if USBIP_TRACE_URBS
#define TRACE_URB(x, ...) DEBUG_USB(x, ##__VA_ARGS__)
#else
#define TRACE_URB(x, ...)
#endif

// Hold-off (ms) for coalescing DTR/RTS changes to USB-serial adapters. Must
// stay well below esptool's 50 ms post-reset wait so the intended states are
// never merged, and above one WiFi/USB-IP round trip so the unintended
// intermediate one is. 0 disables coalescing.
#ifndef USBIP_LINE_STATE_HOLD_MS
#define USBIP_LINE_STATE_HOLD_MS 25
#endif

namespace usbip {

#define BULK_MPS 64  // Full-Speed bulk max packet size

// USB standard requests
static constexpr uint8_t USB_REQ_GET_STATUS = 0x00;
static constexpr uint8_t USB_REQ_CLEAR_FEATURE = 0x01;
static constexpr uint8_t USB_REQ_SET_FEATURE = 0x03;
static constexpr uint8_t USB_REQ_SET_ADDRESS = 0x05;
static constexpr uint8_t USB_REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t USB_REQ_SET_DESCRIPTOR = 0x07;
static constexpr uint8_t USB_REQ_GET_CONFIGURATION = 0x08;
static constexpr uint8_t USB_REQ_SET_CONFIGURATION = 0x09;
static constexpr uint8_t USB_REQ_GET_INTERFACE = 0x0A;
static constexpr uint8_t USB_REQ_SET_INTERFACE = 0x0B;

// USB descriptor types
static constexpr uint8_t USB_DESC_DEVICE = 0x01;
static constexpr uint8_t USB_DESC_CONFIGURATION = 0x02;
static constexpr uint8_t USB_DESC_STRING = 0x03;
static constexpr uint8_t USB_DESC_INTERFACE = 0x04;
static constexpr uint8_t USB_DESC_ENDPOINT = 0x05;

// Endpoint transfer types (bmAttributes bits 1:0)
static constexpr uint8_t EP_TYPE_CONTROL = 0;
static constexpr uint8_t EP_TYPE_ISO = 1;
static constexpr uint8_t EP_TYPE_BULK = 2;
static constexpr uint8_t EP_TYPE_INTERRUPT = 3;

// The ECU tap (see ecu_tap.cpp) was written assuming vendor bulk transfers;
// some devices (this Ignitron ECU included) poll over an interrupt endpoint
// instead, so gate the tap on "not control/iso" rather than "bulk only" —
// otherwise every transfer is silently invisible to the capture tool.
static inline bool is_tappable_ep_type(uint8_t t) {
  return t == EP_TYPE_BULK || t == EP_TYPE_INTERRUPT;
}

// Linux URB transfer_flags carried by USB/IP
static constexpr uint32_t URB_SHORT_NOT_OK = 0x0001;
static constexpr uint32_t URB_ZERO_PACKET = 0x0040;

// Global component reference for callbacks
USBIPComponent *g_usbip_component = nullptr;

// USB Host library client handle
static usb_host_client_handle_t usb_client_hdl = nullptr;
static usb_device_handle_t usb_device_hdl = nullptr;
static SemaphoreHandle_t usb_event_sem = nullptr;

// Synchronous control helper for enumeration-time requests (SET_CONFIGURATION).
// The client-event task is the sole callback pump; we just block on a semaphore.
static SemaphoreHandle_t sync_ctrl_sem = nullptr;
static void sync_ctrl_cb(usb_transfer_t *t) {
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
}
// The handshake replay (replay_ecu_handshake) runs on the web server's task,
// so it has its own completion semaphore — sync_ctrl_sem belongs to the
// bridge task — and both helpers take s_ctrl_mtx so they never have ep0
// transfers in flight at the same time.
static SemaphoreHandle_t s_replay_sem = nullptr;
static SemaphoreHandle_t s_ctrl_mtx = nullptr;

// --- Pending URB table --------------------------------------------------------
// Ported from DatanoiseTV/usbip-esp32p4 transfer_engine.c (proven with HID and
// mass-storage) plus the collateral-cancel resubmit from yunsmall/usbipdcpp's
// Esp32DeviceHandler (proven on ESP32-S3). Every CMD_SUBMIT allocates its own
// usb_transfer_t, is submitted immediately and parked here; completions are
// polled from loop() and answered in completion order. No persistent
// per-endpoint transfers, no request FIFOs, no event-pump mutex.
//
// URBs have NO host-side timeout: an interrupt-IN URB on an idle keyboard or a
// bulk-IN URB on a serial adapter waiting for data legitimately stays pending
// for hours. The client owns the lifetime (CMD_UNLINK); only orphaned slots
// (client gone, callback never fired) are reaped here.
static constexpr int kMaxPendingUrbs = 32;
static constexpr uint32_t kMaxTransferSize = 16384;  // per-USB-transfer (DMA) chunk
static constexpr uint32_t kMaxUrbInSize = 65536;     // largest IN URB (parked in <=16 KB heap chunks)
static constexpr int kMaxInChunks = 8;               // >= kMaxUrbInSize / smallest mps-aligned chunk
static constexpr uint32_t kMaxUrbOutSize = 4u << 20; // OUT payloads stream from the socket; sanity cap
static constexpr int64_t kOrphanRetryUs = 5LL * 1000 * 1000;  // re-try aborting orphaned slots

// Linux errno values (USB/IP clients expect these, not ESP-IDF codes)
static constexpr int32_t LINUX_EIO = 5;
static constexpr int32_t LINUX_ENODEV = 19;
static constexpr int32_t LINUX_EINVAL = 22;
static constexpr int32_t LINUX_EPIPE = 32;
static constexpr int32_t LINUX_EOVERFLOW = 75;
static constexpr int32_t LINUX_ECONNRESET = 104;
static constexpr int32_t LINUX_ESHUTDOWN = 108;
static constexpr int32_t LINUX_ETIMEDOUT = 110;
static constexpr int32_t LINUX_EREMOTEIO = 121;

struct PendingUrb {
  volatile bool active{false};
  volatile bool done{false};   // callback fired; status/actual are valid
  bool unlinked{false};        // CMD_UNLINK seen: never send RET_SUBMIT
  bool killed{false};          // endpoint torn down under it: answer -ESHUTDOWN, never resubmit
  bool short_not_ok{false};    // URB_SHORT_NOT_OK: a short IN read is an error (-EREMOTEIO)
  uint32_t seqnum{0};
  uint32_t direction{0};       // 1 = IN
  uint8_t ep_addr{0};          // full address incl. 0x80 for IN
  uint8_t ep_type{EP_TYPE_BULK};
  uint32_t buflen{0};          // client-requested transfer_buffer_length
  uint16_t wlength{0};         // control only: wLength from the setup packet
  bool is_control{false};
  int64_t submit_us{0};
  int64_t done_us{0};    // latency diagnostic: set when the callback fires
  usb_transfer_t *xfer{nullptr};
  volatile usb_transfer_status_t status{USB_TRANSFER_STATUS_ERROR};
  volatile int actual{0};
  // Chunked large-URB state (buflen > kMaxTransferSize): the transfer moves
  // through mps-aligned 16 KB DMA transfers. IN payloads are parked as the
  // completed transfers themselves — each finished usb_transfer_t is kept and
  // a fresh one allocated for the next chunk — because RET_SUBMIT needs the
  // final length before any data can go out. Never one contiguous 64 KB
  // block: a heap fragmented by WiFi/lwIP churn can't always provide it, and
  // that failure surfaced to Windows as "an error with this drive". OUT
  // payloads are streamed straight from the TCP socket chunk by chunk.
  bool chunked_in{false};
  usb_transfer_t *in_chunk[kMaxInChunks]{};
  uint32_t in_chunk_len[kMaxInChunks]{};
  uint8_t in_nchunks{0};
  bool stream_out{false};      // OUT payload still being consumed from the socket
  uint32_t accum_off{0};       // bytes completed so far
  uint32_t chunk_bytes{0};     // num_bytes of the chunk currently in flight
  uint16_t mps{64};
  bool want_zlp{false};        // URB_ZERO_PACKET requested (OUT, final chunk only)
};
static PendingUrb s_pending[kMaxPendingUrbs];

// The OUT URB currently owning the TCP stream (its payload is still arriving),
// or nullptr. While set, no further USB/IP headers are parsed.
static PendingUrb *s_stream_urb = nullptr;

// Pending (acked-but-not-yet-applied) DTR/RTS state for USB-serial adapters;
// see the modem-line coalescing block further down.
enum LineStateKind : uint8_t { LS_NONE, LS_WHOLE, LS_MASKED };
struct LineState {
  bool pending{false};
  uint8_t setup[8];
  int64_t first_us{0};
};
static LineState s_line_state;

// Completion callback — runs in the client-event task. Records results only;
// replies are sent from loop() (reference-engine pattern).
static void urb_transfer_cb(usb_transfer_t *t) {
  PendingUrb *p = static_cast<PendingUrb *>(t->context);
  if (!p) return;
  p->status = t->status;
  p->actual = t->actual_num_bytes;
  p->done_us = esp_timer_get_time();  // latency diagnostic
  p->done = true;  // written last: poller reads status/actual only after done
}

static PendingUrb *find_free_urb_slot() {
  for (auto &p : s_pending)
    if (!p.active) return &p;
  return nullptr;
}

static PendingUrb *find_urb_by_seqnum(uint32_t seqnum) {
  for (auto &p : s_pending)
    if (p.active && p.seqnum == seqnum) return &p;
  return nullptr;
}

static void free_urb_slot(PendingUrb *p) {
  if (s_stream_urb == p) s_stream_urb = nullptr;
  if (p->xfer) {
    usb_host_transfer_free(p->xfer);
    p->xfer = nullptr;
  }
  for (int i = 0; i < p->in_nchunks; i++) {
    usb_host_transfer_free(p->in_chunk[i]);
    p->in_chunk[i] = nullptr;
    p->in_chunk_len[i] = 0;
  }
  p->in_nchunks = 0;
  p->chunked_in = false;
  p->stream_out = false;
  p->accum_off = 0;
  p->chunk_bytes = 0;
  p->active = false;
}

static int32_t map_usb_status(usb_transfer_status_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return 0;
    case USB_TRANSFER_STATUS_ERROR:     return -LINUX_EIO;
    case USB_TRANSFER_STATUS_TIMED_OUT: return -LINUX_ETIMEDOUT;
    case USB_TRANSFER_STATUS_CANCELED:  return -LINUX_ECONNRESET;
    case USB_TRANSFER_STATUS_STALL:     return -LINUX_EPIPE;
    case USB_TRANSFER_STATUS_NO_DEVICE: return -LINUX_ENODEV;
    case USB_TRANSFER_STATUS_OVERFLOW:  return -LINUX_EOVERFLOW;
    default:                            return -LINUX_EIO;
  }
}

// Wait (yielding) until a pending URB's callback has fired, or the deadline.
static bool wait_urb_done(PendingUrb *p, uint32_t timeout_ms) {
  uint32_t t0 = millis();
  while (!p->done && millis() - t0 < timeout_ms) vTaskDelay(1);
  return p->done;
}

// Submit a control transfer, tolerating the brief window after a device STALL
// in which usbh has dequeued the stalled URB (our callback already fired) but
// not yet re-activated ep0 — that happens asynchronously in the daemon task
// and would otherwise surface to the client as a spurious -EPIPE.
static esp_err_t submit_control_retry(usb_transfer_t *xfer) {
  esp_err_t err = ESP_FAIL;
  for (int i = 0; i < 20; i++) {
    err = usb_host_transfer_submit_control(usb_client_hdl, xfer);
    if (err != ESP_ERR_INVALID_STATE) break;
    vTaskDelay(1);
  }
  return err;
}

// Abort one pending URB (Linux usb_unlink_urb semantics: immediate). The
// endpoint halt/flush cancels every URB queued on that pipe; the collateral
// victims are resubmitted in seqnum order by resubmit_canceled_urbs(). ep0
// URBs can't be individually canceled on ESP-IDF — they're left to complete
// (or fail on unplug) and reaped silently by the poll loop.
static void cancel_pending_urb(PendingUrb *p, usb_device_handle_t dev) {
  if (p->done) return;
  if (p->is_control || !dev) return;
  usb_host_endpoint_halt(dev, p->ep_addr);
  usb_host_endpoint_flush(dev, p->ep_addr);
  wait_urb_done(p, 1000);
  usb_host_endpoint_clear(dev, p->ep_addr);
}

// USB Host daemon task handle
static TaskHandle_t usb_daemon_task_hdl = nullptr;
static volatile bool usb_daemon_running = false;

// Enumeration filter callback - required when CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK is enabled
// Returns true to allow enumeration of the device, false to reject it
// This callback fires BEFORE the USB_HOST_CLIENT_EVENT_NEW_DEV callback
static bool enum_filter_callback(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  DEBUG_USB("Enum filter: VID=%04X PID=%04X Class=%02X",
           dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);
  // Use configuration value 1 (most common)
  *bConfigurationValue = 1;
  return true;  // Allow all devices
}

// Pending device actions (set by callback, processed by main loop)
static volatile uint8_t pending_dev_addr = 0;
static volatile bool pending_dev_connect = false;
static volatile bool pending_dev_disconnect = false;

// Device discovery request (set by daemon task, processed by main loop)
// This avoids calling usb_host_device_open() from within the daemon task
static volatile bool request_device_discovery = false;

// Port power state - start unpowered, power on after client is registered
static volatile bool port_power_requested = false;
static volatile bool port_powered = false;

// USB Host daemon task - continuously processes USB events in background
static void usb_host_daemon_task(void *arg) {
  DEBUG_USB("USB Host daemon task started");
  usb_daemon_running = true;

  static uint32_t startup_time = 0;
  static bool initial_discovery_done = false;

  // Record startup time for enumeration wait
  startup_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

  while (usb_daemon_running) {
    // Library-level events only; client events (transfer callbacks) have their
    // own dedicated task, matching the reference implementations.
    uint32_t event_flags = 0;
    esp_err_t err = usb_host_lib_handle_events(50, &event_flags);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
      DEBUG_USB("usb_host_lib_handle_events error: %s", esp_err_to_name(err));

    // Log library events
    if (event_flags != 0) {
      DEBUG_USB("USB lib event flags: 0x%lX", (unsigned long)event_flags);
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Power on the port 500ms after startup (once client is registered)
    // This ensures enumeration happens AFTER our client is ready to receive NEW_DEV callback
    if (port_power_requested && !port_powered && (now - startup_time) > 500) {
      DEBUG_USB("Powering on USB root port...");
      err = usb_host_lib_set_root_port_power(true);
      if (err == ESP_OK) {
        DEBUG_USB("USB root port powered ON - waiting for device enumeration");
        port_powered = true;
      } else {
        DEBUG_USB("Failed to power on root port: %s", esp_err_to_name(err));
      }
    }

    // Wait 5 seconds after port power-on before checking for manual discovery
    // This gives time for device enumeration via the normal callback path
    if (port_powered && !initial_discovery_done && (now - startup_time) > 5500) {
      initial_discovery_done = true;

      usb_host_lib_info_t lib_info;
      if (usb_host_lib_info(&lib_info) == ESP_OK) {
        DEBUG_USB("Enumeration check: %d devices in library, handle=%p",
                 lib_info.num_devices, usb_device_hdl);

        // Only request manual discovery if device in library but callback never fired
        if (lib_info.num_devices > 0 && usb_device_hdl == nullptr && !pending_dev_connect) {
          DEBUG_USB("Device in library but NEW_DEV callback never fired - requesting manual discovery");
          request_device_discovery = true;
        }
      }
    }

    esp_task_wdt_reset();
  }

  DEBUG_USB("USB Host daemon task stopped");
  vTaskDelete(NULL);
}

// Client event pump — the ONLY place usb_host_client_handle_events() runs.
// Transfer completion callbacks execute in this task's context.
static TaskHandle_t usb_client_task_hdl = nullptr;
static void usb_client_events_task(void *arg) {
  DEBUG_USB("USB client event task started");
  while (usb_daemon_running) {
    if (usb_client_hdl)
      usb_host_client_handle_events(usb_client_hdl, 1000);
    else
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  DEBUG_USB("USB client event task stopped");
  vTaskDelete(NULL);
}

// USB Host event callback - called from usb_host_client_handle_events context
// IMPORTANT: Do not block or do heavy work here - just set flags for daemon task
static void usb_host_client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg) {
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      DEBUG_USB("USB_HOST_CLIENT_EVENT_NEW_DEV: address=%d", event_msg->new_dev.address);
      pending_dev_addr = event_msg->new_dev.address;
      pending_dev_connect = true;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      DEBUG_USB("USB_HOST_CLIENT_EVENT_DEV_GONE");
      pending_dev_disconnect = true;
      break;
    default:
      DEBUG_USB("Unknown USB client event: %d", event_msg->event);
      break;
  }
  if (usb_event_sem) {
    xSemaphoreGive(usb_event_sem);
  }
}

USBIPComponent::USBIPComponent() {
  g_usbip_component = this;
}

USBIPComponent::~USBIPComponent() {
  // Stop USB daemon task first
  if (usb_daemon_task_hdl) {
    usb_daemon_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
    usb_daemon_task_hdl = nullptr;
  }

  if (this->client_fd_ >= 0) {
    close(this->client_fd_);
  }
  if (this->server_fd_ >= 0) {
    close(this->server_fd_);
  }
  if (usb_client_hdl) {
    usb_host_client_deregister(usb_client_hdl);
    usb_client_hdl = nullptr;
  }
  if (usb_event_sem) {
    vSemaphoreDelete(usb_event_sem);
    usb_event_sem = nullptr;
  }
  g_usbip_component = nullptr;
}

void USBIPComponent::setup() {
  // Initialize USB Host
  this->usb_host_init_();

  // Start TCP server
  this->start_server_();
}

void USBIPComponent::usb_host_init_() {

  // Create semaphore for USB events
  usb_event_sem = xSemaphoreCreateBinary();
  if (!usb_event_sem) {
    DEBUG_USB("Failed to create USB event semaphore");
    return;
  }

  // Semaphore for the enumeration-time synchronous control helper
  sync_ctrl_sem = xSemaphoreCreateBinary();
  if (!sync_ctrl_sem) {
    DEBUG_USB("Failed to create sync control semaphore");
    return;
  }

  // Install USB Host library
  // IMPORTANT: Start with port unpowered so enumeration happens AFTER client registration
  // This ensures the NEW_DEV callback fires to our client, enabling proper interface claiming
  // CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK is enabled, so we provide a callback
  usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .root_port_unpowered = true,  // Start unpowered, power on after client is registered
      .intr_flags = ESP_INTR_FLAG_LOWMED,
      .enum_filter_cb = enum_filter_callback,  // Required when ENABLE_ENUM_FILTER_CALLBACK is set
      .fifo_settings_custom = {0, 0, 0},       // Use Kconfig defaults
      .peripheral_map = 0,                     // Use default peripheral
  };

  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    DEBUG_USB("Failed to install USB Host library: %s", esp_err_to_name(err));
    return;
  }

  DEBUG_USB("USB Host library installed");

  // Register USB Host client
  usb_host_client_config_t client_config = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async = {
          .client_event_callback = usb_host_client_event_callback,
          .callback_arg = nullptr,
      },
  };

  err = usb_host_client_register(&client_config, &usb_client_hdl);
  if (err != ESP_OK) {
    DEBUG_USB("Failed to register USB Host client: %s", esp_err_to_name(err));
    usb_host_uninstall();
    return;
  }

  // Start USB Host daemon task for continuous event processing
  // Stack size 8192 needed for device enumeration with logging
  // Set the run flag BEFORE creating either task: the client task checks it on
  // entry and would exit instantly (killing all event delivery) if it ran
  // before the daemon task's own assignment.
  usb_daemon_running = true;
  BaseType_t task_created = xTaskCreatePinnedToCore(
      usb_host_daemon_task,
      "usb_daemon",
      8192,
      nullptr,
      5,  // Higher priority than main loop
      &usb_daemon_task_hdl,
      0   // Pin to core 0
  );
  if (task_created != pdTRUE) {
    DEBUG_USB("Failed to create USB daemon task");
  }

  // Dedicated client-event pump: transfer callbacks fire here with minimal
  // latency, independent of the library event loop (reference-repo pattern).
  task_created = xTaskCreatePinnedToCore(
      usb_client_events_task,
      "usb_client",
      4096,
      nullptr,
      6,  // above the daemon so completions are delivered promptly
      &usb_client_task_hdl,
      0);
  if (task_created != pdTRUE) {
    DEBUG_USB("Failed to create USB client event task");
  }

  this->usb_host_client_hdl_ = usb_client_hdl;
  this->usb_host_initialized_ = true;
  // Request port power-on (daemon task will handle it after a delay)
  // This ensures enumeration happens after our client is registered
  port_power_requested = true;
  DEBUG_USB("USB Host initialized");
}

void USBIPComponent::start_server_() {

  this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->server_fd_ < 0) {
    DEBUG_USB("Failed to create socket: %d", errno);
    return;
  }

  // Set socket options
  int opt = 1;
  setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Set non-blocking
  int flags = fcntl(this->server_fd_, F_GETFL, 0);
  fcntl(this->server_fd_, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(this->port_);

  if (bind(this->server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    DEBUG_USB("Failed to bind socket: %d", errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  if (listen(this->server_fd_, 1) < 0) {
    DEBUG_USB("Failed to listen: %d", errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  DEBUG_USB("USB/IP server listening on port %d", this->port_);
}

void USBIPComponent::loop() {
  // Handle device discovery request from daemon task
  // IMPORTANT: usb_host_device_open() must be called from main loop, not daemon task
  // ESP-IDF 5.5.x changed internal state management which causes crashes if called from daemon
  if (request_device_discovery && usb_client_hdl && usb_device_hdl == nullptr) {
    request_device_discovery = false;
    DEBUG_USB("Processing device discovery request from main loop...");

    for (uint8_t addr = 1; addr <= 10; addr++) {
      usb_device_handle_t dev_hdl = nullptr;
      esp_err_t open_err = usb_host_device_open(usb_client_hdl, addr, &dev_hdl);
      if (open_err == ESP_OK && dev_hdl != nullptr) {
        DEBUG_USB("Opened device at address %d, handle=%p", addr, dev_hdl);
        usb_device_hdl = dev_hdl;
        pending_dev_addr = addr;
        pending_dev_connect = true;
        break;
      } else if (open_err != ESP_ERR_NOT_FOUND) {
        DEBUG_USB("Address %d: %s", addr, esp_err_to_name(open_err));
      }
    }
  }

  // Handle pending device connect/disconnect (set by callback or discovery)
  // Process disconnect FIRST to clean up old device before opening new one
  if (pending_dev_disconnect) {
    pending_dev_disconnect = false;
    crumb::set(crumb::BRIDGE_DEVICE);
    DEBUG_USB("Processing device disconnect");
    this->on_device_disconnected(0);
  }
  if (pending_dev_connect) {
    uint8_t addr = pending_dev_addr;
    pending_dev_connect = false;
    crumb::set(crumb::BRIDGE_DEVICE);
    DEBUG_USB("Processing device connect for address %d", addr);
    this->on_device_connected(addr);
  }

  // Handle TCP client connections
  crumb::set(crumb::BRIDGE_CLIENT);
  if (this->server_fd_ >= 0) {
    // Check for new clients
    if (this->client_fd_ < 0) {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int new_client = accept(this->server_fd_, (struct sockaddr *)&client_addr, &client_len);
      if (new_client >= 0) {
        DEBUG_USB("New USB/IP client connected");
        this->client_fd_ = new_client;
        this->client_state_ = CLIENT_STATE_IDLE;

        // Enable TCP_NODELAY for immediate packet transmission
        int opt = 1;
        setsockopt(this->client_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Enlarge the socket buffers so large URB replies stream without
        // stalling on a full send window (each stall costs a 1 ms task yield).
        int bufsz = 64 * 1024;
        setsockopt(this->client_fd_, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
        setsockopt(this->client_fd_, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

        // Set client socket to non-blocking
        int flags = fcntl(this->client_fd_, F_GETFL, 0);
        fcntl(this->client_fd_, F_SETFL, flags | O_NONBLOCK);
      }
    }

    // Handle existing client
    if (this->client_fd_ >= 0) {
      this->handle_client_();
    }
  }

  // Answer completed URBs; also reaps unlinked/orphaned slots after disconnects.
  crumb::set(crumb::BRIDGE_POLL);
  this->poll_pending_urbs_();
}

void USBIPComponent::on_device_connected(uint8_t dev_addr) {
  DEBUG_USB("on_device_connected: addr=%d, usb_device_hdl=%p", dev_addr, usb_device_hdl);

  // Open the device if not already open
  if (usb_device_hdl == nullptr) {
    esp_err_t err = usb_host_device_open(usb_client_hdl, dev_addr, &usb_device_hdl);
    if (err != ESP_OK) {
      DEBUG_USB("Failed to open USB device: %s", esp_err_to_name(err));
      return;
    }
    DEBUG_USB("Device opened, handle=%p", usb_device_hdl);
  }

  this->usb_device_.connected = true;
  this->usb_device_.dev_addr = dev_addr;
  this->refresh_device_info_();

  // Enumerate the device
  if (!this->enumerate_device_(dev_addr)) {
    DEBUG_USB("Failed to enumerate device");
    this->refresh_device_info_();
    return;
  }

  this->usb_device_.enumerated = true;
  this->refresh_device_info_();
  DEBUG_USB("Device enumerated: VID=%04X PID=%04X Class=%02X",
           this->usb_device_.vid, this->usb_device_.pid, this->usb_device_.device_class);
}

void USBIPComponent::on_device_disconnected(uint8_t dev_addr) {
  DEBUG_USB("USB device disconnected");

  // The stack halts+flushes every endpoint on unplug, so each pending URB's
  // callback fires (NO_DEVICE/CANCELED). Wait for it, answer -ENODEV, free.
  for (auto &p : s_pending) {
    if (!p.active) continue;
    if (!wait_urb_done(&p, 2000)) {
      DEBUG_USB("URB seq=%lu still in flight at unplug — deferring reap",
               (unsigned long)p.seqnum);
      p.unlinked = true;  // poll loop frees it silently when the callback lands
      continue;
    }
    if (!p.unlinked && this->client_fd_ >= 0 && this->client_state_ == CLIENT_STATE_ATTACHED)
      this->send_urb_response_(p.seqnum, -LINUX_ENODEV, nullptr, 0);
    free_urb_slot(&p);
  }

  if (usb_device_hdl) {
    this->release_all_interfaces_();
    usb_host_device_close(usb_client_hdl, usb_device_hdl);
    usb_device_hdl = nullptr;
  }

  this->usb_device_.connected = false;
  this->usb_device_.enumerated = false;
  this->usb_device_.dev_addr = 0;
  this->usb_device_.device_descriptor.clear();
  this->usb_device_.clear_config();
  this->refresh_device_info_();
  s_line_state.pending = false;

  // Close TCP connection so the Linux USB/IP client detects the disconnect
  if (this->client_state_ == CLIENT_STATE_ATTACHED) {
    this->disconnect_client_();
  }
}

void USBIPComponent::refresh_device_info_() {
  DeviceInfo d{};
  d.present = this->usb_device_.connected;
  d.ready = this->usb_device_.enumerated;
  d.vid = this->usb_device_.vid;
  d.pid = this->usb_device_.pid;
  // Class lives on the device descriptor for simple devices, but composite
  // devices (0x00) define it per-interface — fall back to the first interface.
  d.usb_class = this->usb_device_.device_class;
  if (d.usb_class == 0 && !this->usb_device_.interfaces.empty())
    d.usb_class = this->usb_device_.interfaces[0].interface_class;
  this->dev_info_cache_ = d;
}

// Walk the cached configuration descriptor and record every interface
// descriptor (all alternate settings) with its endpoints. Class-specific and
// interface-association descriptors are skipped; endpoints belong to the most
// recent interface descriptor, which is what the USB spec mandates.
bool USBIPComponent::parse_config_descriptor_() {
  USBDevice &d = this->usb_device_;
  d.interfaces.clear();
  d.iface_numbers.clear();
  d.iface_alt.clear();
  d.iface_claimed.clear();

  const std::vector<uint8_t> &cfg = d.config_descriptor;
  if (cfg.size() < 9) return false;
  size_t total = cfg.size();
  size_t offset = cfg[0];
  while (offset + 2 <= total) {
    uint8_t len = cfg[offset];
    uint8_t type = cfg[offset + 1];
    if (len < 2 || offset + len > total) break;

    if (type == USB_DESC_INTERFACE && len >= 9) {
      InterfaceInfo it;
      it.interface_number = cfg[offset + 2];
      it.alt_setting = cfg[offset + 3];
      it.interface_class = cfg[offset + 5];
      it.interface_subclass = cfg[offset + 6];
      it.interface_protocol = cfg[offset + 7];
      d.interfaces.push_back(it);
      DEBUG_USB("  Interface %d alt %d: Class=%02X SubClass=%02X Protocol=%02X",
               it.interface_number, it.alt_setting, it.interface_class,
               it.interface_subclass, it.interface_protocol);
    } else if (type == USB_DESC_ENDPOINT && len >= 7 && !d.interfaces.empty()) {
      EndpointInfo ep;
      ep.address = cfg[offset + 2];
      ep.attributes = cfg[offset + 3];
      ep.max_packet_size = (uint16_t)(cfg[offset + 4] | (cfg[offset + 5] << 8)) & 0x07FF;
      ep.interval = cfg[offset + 6];
      d.interfaces.back().endpoints.push_back(ep);
      DEBUG_USB("    Endpoint %02X: Attr=%02X MaxPacket=%d Interval=%d",
               ep.address, ep.attributes, ep.max_packet_size, ep.interval);
    }
    offset += len;
  }

  for (auto &it : d.interfaces) {
    if (d.iface_index(it.interface_number) < 0) {
      d.iface_numbers.push_back(it.interface_number);
      d.iface_alt.push_back(0);
      d.iface_claimed.push_back(false);
    }
  }
  return !d.iface_numbers.empty();
}

bool USBIPComponent::enumerate_device_(uint8_t dev_addr) {
  if (!usb_device_hdl) {
    return false;
  }

  // Feed watchdog before potentially long operations
  esp_task_wdt_reset();

  // Give USB stack time to fully initialize device after open
  // ESP-IDF 5.5.x requires more settling time
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_task_wdt_reset();

  // Get device descriptor
  const usb_device_desc_t *dev_desc;
  esp_err_t err = usb_host_get_device_descriptor(usb_device_hdl, &dev_desc);
  if (err != ESP_OK) {
    DEBUG_USB("Failed to get device descriptor: %s", esp_err_to_name(err));
    return false;
  }

  esp_task_wdt_reset();

  // Store device info
  this->usb_device_.vid = dev_desc->idVendor;
  this->usb_device_.pid = dev_desc->idProduct;
  this->usb_device_.bcd_device = dev_desc->bcdDevice;
  this->usb_device_.device_class = dev_desc->bDeviceClass;
  this->usb_device_.device_subclass = dev_desc->bDeviceSubClass;
  this->usb_device_.device_protocol = dev_desc->bDeviceProtocol;
  this->usb_device_.max_packet_size0 = dev_desc->bMaxPacketSize0 ? dev_desc->bMaxPacketSize0 : 8;
  this->usb_device_.num_configurations = dev_desc->bNumConfigurations;

  // Store raw device descriptor
  this->usb_device_.device_descriptor.assign(
      reinterpret_cast<const uint8_t *>(dev_desc),
      reinterpret_cast<const uint8_t *>(dev_desc) + sizeof(usb_device_desc_t));

  DEBUG_USB("Device: VID=%04X PID=%04X Class=%02X MPS0=%d",
           this->usb_device_.vid, this->usb_device_.pid, this->usb_device_.device_class,
           this->usb_device_.max_packet_size0);

  // Allow USB events to be processed before getting config descriptor
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_task_wdt_reset();

  // Get configuration descriptor
  // In ESP-IDF 5.4.0+, multiconfiguration support was added
  // usb_host_get_active_config_descriptor() may return nullptr if no config is active yet
  const usb_config_desc_t *config_desc = nullptr;
  err = usb_host_get_active_config_descriptor(usb_device_hdl, &config_desc);

  if (err == ESP_OK && config_desc == nullptr) {
    // No active configuration yet - try to get config descriptor by configuration value
    // Most devices use bConfigurationValue = 1 for first configuration
    DEBUG_USB("No active config, trying to get config descriptor for config value 1");
    err = usb_host_get_config_desc(usb_client_hdl, usb_device_hdl, 1, &config_desc);
  }

  if (err != ESP_OK) {
    DEBUG_USB("Failed to get config descriptor: %s", esp_err_to_name(err));
    return false;
  }

  if (config_desc == nullptr) {
    DEBUG_USB("Config descriptor is NULL even after fallback");
    return false;
  }

  esp_task_wdt_reset();

  // Validate config descriptor
  uint16_t total_length = config_desc->wTotalLength;
  if (total_length < sizeof(usb_config_desc_t) || total_length > 4096) {
    DEBUG_USB("Invalid config descriptor length: %d", total_length);
    return false;
  }

  // Store raw config descriptor
  this->usb_device_.config_descriptor.assign(
      reinterpret_cast<const uint8_t *>(config_desc),
      reinterpret_cast<const uint8_t *>(config_desc) + total_length);

  this->usb_device_.current_configuration = config_desc->bConfigurationValue;

  DEBUG_USB("Config %d: %d interfaces, %d bytes", config_desc->bConfigurationValue,
           config_desc->bNumInterfaces, total_length);

  esp_task_wdt_reset();

  if (!this->parse_config_descriptor_())
    DEBUG_USB("Config descriptor has no interfaces");

  // Determine device speed (convert ESP-IDF speed to Linux USB/IP speed values)
  usb_device_info_t dev_info;
  err = usb_host_device_info(usb_device_hdl, &dev_info);
  if (err == ESP_OK) {
    switch (dev_info.speed) {
      case USB_SPEED_LOW:  this->usb_device_.speed = USBIP_SPEED_LOW; break;
      case USB_SPEED_HIGH: this->usb_device_.speed = USBIP_SPEED_HIGH; break;
      default:             this->usb_device_.speed = USBIP_SPEED_FULL; break;
    }
    if (dev_info.bMaxPacketSize0)
      this->usb_device_.max_packet_size0 = dev_info.bMaxPacketSize0;
    DEBUG_USB("Device speed: ESP-IDF=%d -> USBIP=%lu",
             dev_info.speed, (unsigned long)this->usb_device_.speed);
  }

  esp_task_wdt_reset();

  // If device has no active configuration, we need to set it first
  // This is required in ESP-IDF 5.4.0+ when using usb_host_get_config_desc()
  const usb_config_desc_t *active_config = nullptr;
  err = usb_host_get_active_config_descriptor(usb_device_hdl, &active_config);
  if (err == ESP_OK && active_config == nullptr) {
    DEBUG_USB("Setting configuration %d...", this->usb_device_.current_configuration);
    uint8_t setup[8] = {0x00, USB_REQ_SET_CONFIGURATION,
                        this->usb_device_.current_configuration, 0x00,
                        0x00, 0x00, 0x00, 0x00};
    esp_err_t serr = this->sync_control_out_(setup);
    DEBUG_USB("SET_CONFIGURATION: %s", esp_err_to_name(serr));
    // Let the device settle; the daemon/client tasks keep pumping events.
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_task_wdt_reset();
  }

  // Verify configuration is now active
  active_config = nullptr;
  err = usb_host_get_active_config_descriptor(usb_device_hdl, &active_config);
  if (active_config != nullptr) {
    DEBUG_USB("Active configuration confirmed: %d interfaces", active_config->bNumInterfaces);
  } else {
    DEBUG_USB("Library reports no active config (will try claiming anyway)");
  }

  this->usb_device_.configured = true;
  this->claim_all_interfaces_();
  return true;
}

// --- Interface / pipe lifecycle ---------------------------------------------

bool USBIPComponent::claim_interface_(uint8_t number, uint8_t alt) {
  int idx = this->usb_device_.iface_index(number);
  if (idx < 0 || !usb_client_hdl || !usb_device_hdl) return false;
  esp_err_t err = usb_host_interface_claim(usb_client_hdl, usb_device_hdl, number, alt);
  if (err == ESP_OK) {
    this->usb_device_.iface_claimed[idx] = true;
    this->usb_device_.iface_alt[idx] = alt;
    return true;
  }
  // Every pipe needs a DWC host channel (8 on the S3, one taken by ep0): a
  // device with more than 7 data endpoints can only be partially served.
  DEBUG_USB("Claim interface %d alt %d failed: %s", number, alt, esp_err_to_name(err));
  this->usb_device_.iface_claimed[idx] = false;
  return false;
}

// Release retries briefly: the client-event task clears an endpoint's
// in-flight count only after running our completion callback, so a release
// issued the instant a URB reports done can still see ESP_ERR_INVALID_STATE.
bool USBIPComponent::release_interface_(uint8_t number) {
  int idx = this->usb_device_.iface_index(number);
  if (idx < 0 || !usb_client_hdl || !usb_device_hdl) return false;
  esp_err_t err = ESP_OK;
  for (int i = 0; i < 100; i++) {
    err = usb_host_interface_release(usb_client_hdl, usb_device_hdl, number);
    if (err != ESP_ERR_INVALID_STATE) break;
    vTaskDelay(1);
  }
  if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
    this->usb_device_.iface_claimed[idx] = false;
    return true;
  }
  DEBUG_USB("Release interface %d failed: %s", number, esp_err_to_name(err));
  return false;
}

void USBIPComponent::claim_all_interfaces_() {
  USBDevice &d = this->usb_device_;
  DEBUG_USB("Claiming %u interface(s)...", (unsigned)d.iface_numbers.size());
  bool any = false;
  for (size_t i = 0; i < d.iface_numbers.size(); i++) {
    uint8_t num = d.iface_numbers[i];
    // Alternate setting 0 is the one the device is in after SET_CONFIGURATION
    // (fall back to the first alt listed if a non-compliant device lacks it).
    uint8_t alt = 0;
    if (!d.find_interface(num, 0))
      for (auto &it : d.interfaces)
        if (it.interface_number == num) { alt = it.alt_setting; break; }
    if (this->claim_interface_(num, alt)) {
      any = true;
      DEBUG_USB("Claimed interface %d alt %d", num, alt);
    }
    esp_task_wdt_reset();
  }
  if (!any && !d.iface_numbers.empty())
    DEBUG_USB("No interfaces could be claimed - data transfers will fail");
}

void USBIPComponent::release_all_interfaces_() {
  USBDevice &d = this->usb_device_;
  for (size_t i = 0; i < d.iface_numbers.size(); i++)
    this->release_interface_(d.iface_numbers[i]);
}

// Cancel every in-flight URB on the interface's active endpoints and wait for
// their callbacks, so the pipes have no in-flight count and can be freed.
void USBIPComponent::quiesce_interface_(const InterfaceInfo &iface, bool kill) {
  if (!usb_device_hdl) return;
  for (auto &ep : iface.endpoints) {
    bool any = false;
    for (auto &p : s_pending) {
      if (!p.active || p.is_control || p.ep_addr != ep.address) continue;
      if (kill && !(p.done && p.status == USB_TRANSFER_STATUS_COMPLETED))
        p.killed = true;  // never resubmit onto the torn-down endpoint
      if (!p.done) any = true;
    }
    if (any) {
      usb_host_endpoint_halt(usb_device_hdl, ep.address);
      usb_host_endpoint_flush(usb_device_hdl, ep.address);
    }
  }
  for (auto &ep : iface.endpoints)
    for (auto &p : s_pending)
      if (p.active && !p.is_control && p.ep_addr == ep.address && !wait_urb_done(&p, 1000))
        DEBUG_USB("quiesce: URB seq=%lu on ep 0x%02X did not cancel",
                 (unsigned long)p.seqnum, p.ep_addr);
}

void USBIPComponent::quiesce_all_(bool kill) {
  USBDevice &d = this->usb_device_;
  for (size_t i = 0; i < d.iface_numbers.size(); i++) {
    const InterfaceInfo *it = d.active_interface(d.iface_numbers[i]);
    if (it) this->quiesce_interface_(*it, kill);
  }
}

// Destroy and recreate an interface's pipes (release + claim of the current
// alternate setting) so every host-side data toggle restarts at DATA0.
bool USBIPComponent::recreate_interface_pipes_(uint8_t number) {
  int idx = this->usb_device_.iface_index(number);
  if (idx < 0) return false;
  uint8_t alt = this->usb_device_.iface_alt[idx];
  bool released = this->release_interface_(number);
  bool claimed = this->claim_interface_(number, alt);
  if (!released || !claimed)
    DEBUG_USB("Pipe recreate iface %u alt %u: release=%d claim=%d", number, alt, released, claimed);
  return released && claimed;
}

// Resubmit URBs that were canceled as collateral of an endpoint halt (UNLINK
// of a neighbour, pipe recreation) in seqnum order, so the device sees them
// in the order the client issued them. Killed/unlinked slots are left for the
// poll loop, which answers or reaps them.
static void resubmit_canceled_urbs() {
  if (!usb_device_hdl) return;
  for (;;) {
    PendingUrb *next = nullptr;
    for (auto &p : s_pending) {
      if (!p.active || !p.done || p.unlinked || p.killed) continue;
      if (p.status != USB_TRANSFER_STATUS_CANCELED) continue;
      if (!next || (int32_t)(p.seqnum - next->seqnum) < 0) next = &p;
    }
    if (!next) return;
    next->done = false;
    esp_err_t err = next->is_control
                        ? submit_control_retry(next->xfer)
                        : usb_host_transfer_submit(next->xfer);
    if (err != ESP_OK) {
      // Pipe still halted (device STALL awaiting CLEAR_FEATURE) or gone: the
      // poll loop answers it. Mark it as a failed resubmit, not a cancel.
      DEBUG_USB("Resubmit after collateral cancel failed: seq=%lu %s",
               (unsigned long)next->seqnum, esp_err_to_name(err));
      next->status = USB_TRANSFER_STATUS_STALL;
      next->actual = 0;
      next->done = true;
    }
  }
}

void USBIPComponent::dump_config() {
  DEBUG_USB("USB/IP Component:");
  DEBUG_USB("  Port: %d", this->port_);
  DEBUG_USB("  USB Host initialized: %s", this->usb_host_initialized_ ? "yes" : "no");
  if (this->usb_device_.connected) {
    DEBUG_USB("  USB Device: VID=%04X PID=%04X Class=%02X",
                  this->usb_device_.vid, this->usb_device_.pid, this->usb_device_.device_class);
    DEBUG_USB("  Device enumerated: %s", this->usb_device_.enumerated ? "yes" : "no");
  } else {
    DEBUG_USB("  USB Device: not connected");
  }
}

void USBIPComponent::handle_client_() {
  // An OUT URB is still streaming its payload from the socket: the next
  // header is behind that payload, so there is nothing to parse yet.
  if (s_stream_urb) return;

  // Check if data is available
  uint8_t peek;
  int result = recv(this->client_fd_, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
  if (result == 0) {
    // Connection closed
    DEBUG_USB("Client disconnected");
    this->disconnect_client_();
    return;
  }
  if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    DEBUG_USB("Socket error: %d", errno);
    this->disconnect_client_();
    return;
  }
  if (result < 0) {
    return;  // No data available
  }

  // After IMPORT the wire protocol switches to URB format. Drain every URB the
  // client has already pipelined (up to the free-slot budget) in a single pass
  // so the USB device stays saturated. Reading one URB per loop() iteration
  // while poll_pending_urbs_() flushed all completed replies at once starved
  // the pipeline and produced the fill/flush throughput sawtooth. Back off when
  // the pending table is full (back-pressure; poll frees slots) or when the
  // socket has no more data queued.
  if (this->client_state_ == CLIENT_STATE_ATTACHED) {
    for (int i = 0; i < kMaxPendingUrbs; i++) {
      if (!find_free_urb_slot())
        break;
      // Only keep draining while the heap can still back another URB's DMA
      // transfer plus one parked chunk. Otherwise stop and let
      // poll_pending_urbs_() flush completed replies — freeing their chunks —
      // rather than reading a URB we'd have to fail. The first read is always
      // attempted so the loop can't deadlock.
      if (i > 0 &&
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 2 * kMaxTransferSize + 4096)
        break;
      if (!this->handle_urb_() || this->client_fd_ < 0 || s_stream_urb)
        break;
      uint8_t pk;
      if (recv(this->client_fd_, &pk, 1, MSG_PEEK | MSG_DONTWAIT) <= 0)
        break;
    }
    return;
  }

  // Operation command header format: version(2) + command(2) + status(4) = 8 bytes
  USBIPHeader header;
  if (!this->read_bytes_(reinterpret_cast<uint8_t *>(&header), sizeof(header))) {
    DEBUG_USB("Failed to read USB/IP header");
    this->disconnect_client_();
    return;
  }

  uint16_t version = ntohs_(header.version);
  uint16_t command = ntohs_(header.command);

  // Echo the client's protocol version back in our replies. usbip-win2 rejects a
  // device whose OP_REP version doesn't match what it sent, which shows up as
  // "recognised but won't emulate". Ignore a zero version (some clients pad it).
  if (version != 0) this->negotiated_version_ = version;

  DEBUG_USB("USB/IP command: version=0x%04X, command=0x%04X", version, command);

  switch (command) {
    case OP_REQ_DEVLIST:
      DEBUG_USB("DEVLIST request");
      this->handle_devlist_request_();
      break;

    case OP_REQ_IMPORT:
      DEBUG_USB("IMPORT request");
      this->handle_import_request_();
      break;

    default:
      // Unknown op: we can't know its length, so the stream is unrecoverable.
      DEBUG_USB("Unknown USB/IP command: 0x%04X — dropping client", command);
      this->disconnect_client_();
      break;
  }
}

bool USBIPComponent::handle_devlist_request_() {
  // Note: The status field is already part of the header we read, no additional data to read
  this->send_devlist_response_();
  return true;
}

// Fill the USB/IP device record shared by OP_REP_DEVLIST and OP_REP_IMPORT.
static void fill_device_record(USBIPDeviceInfo &dev_info, const USBDevice &d,
                               uint16_t (*hs)(uint16_t), uint32_t (*hl)(uint32_t)) {
  memset(&dev_info, 0, sizeof(dev_info));
  strncpy(dev_info.path, "/sys/devices/pci0000:00/0000:00:01.2/usb1/1-1", sizeof(dev_info.path) - 1);
  strncpy(dev_info.busid, "1-1", sizeof(dev_info.busid) - 1);
  dev_info.busnum = hl(1);
  dev_info.devnum = hl(d.dev_addr);
  dev_info.speed = hl(d.speed);
  dev_info.idVendor = hs(d.vid);
  dev_info.idProduct = hs(d.pid);
  dev_info.bcdDevice = hs(d.bcd_device);
  dev_info.bDeviceClass = d.device_class;
  dev_info.bDeviceSubClass = d.device_subclass;
  dev_info.bDeviceProtocol = d.device_protocol;
  dev_info.bConfigurationValue = d.current_configuration;
  dev_info.bNumConfigurations = d.num_configurations;
  // Exactly this many interface records follow in DEVLIST — must match what
  // we can actually describe (unique interface numbers we parsed).
  dev_info.bNumInterfaces = (uint8_t)std::min<size_t>(d.iface_numbers.size(), 255);
}

void USBIPComponent::send_devlist_response_() {
  bool has_device = this->usb_device_.connected && this->usb_device_.enumerated;

  DEBUG_USB("Preparing DEVLIST response: device=%s", has_device ? "yes" : "no");

  // Send header
  USBIPHeader header;
  header.version = htons_(this->negotiated_version_);
  header.command = htons_(OP_REP_DEVLIST);
  header.status = htonl_(ST_OK);

  if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&header), sizeof(header))) {
    DEBUG_USB("Failed to send DEVLIST header");
    return;
  }

  // Send device count
  uint32_t device_count = has_device ? htonl_(1) : htonl_(0);
  if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&device_count), sizeof(device_count))) {
    DEBUG_USB("Failed to send device count");
    return;
  }

  if (has_device) {
    const USBDevice &d = this->usb_device_;
    USBIPDeviceInfo dev_info;
    fill_device_record(dev_info, d, htons_, htonl_);

    DEBUG_USB("Sending device: VID=%04X PID=%04X interfaces=%d",
             d.vid, d.pid, dev_info.bNumInterfaces);

    if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&dev_info), sizeof(dev_info))) {
      DEBUG_USB("Failed to send device info");
      return;
    }

    // One record per interface number, describing its active alternate
    // setting (what Linux exports from sysfs), falling back to alt 0.
    for (uint8_t i = 0; i < dev_info.bNumInterfaces; i++) {
      uint8_t num = d.iface_numbers[i];
      const InterfaceInfo *it = d.active_interface(num);
      if (!it) it = d.find_interface(num, 0);
      USBIPInterfaceInfo iface_info{};
      if (it) {
        iface_info.bInterfaceClass = it->interface_class;
        iface_info.bInterfaceSubClass = it->interface_subclass;
        iface_info.bInterfaceProtocol = it->interface_protocol;
      }
      iface_info.padding = 0;

      DEBUG_USB("  Interface %d: class=%02X", num, iface_info.bInterfaceClass);

      if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&iface_info), sizeof(iface_info))) {
        DEBUG_USB("Failed to send interface info");
        return;
      }
    }
  }

  DEBUG_USB("Sent DEVLIST response: %d device(s)", has_device ? 1 : 0);
}

bool USBIPComponent::handle_import_request_() {
  char busid[32];
  memset(busid, 0, sizeof(busid));
  if (!this->read_bytes_(reinterpret_cast<uint8_t *>(busid), sizeof(busid))) {
    DEBUG_USB("Failed to read busid");
    this->disconnect_client_();
    return false;
  }

  // Ensure null termination
  busid[31] = '\0';
  DEBUG_USB("Import request for bus ID: '%s'", busid);

  bool has_device = this->usb_device_.connected && this->usb_device_.enumerated;
  bool success = has_device && (strcmp(busid, "1-1") == 0);

  DEBUG_USB("Import: has_device=%d, busid_match=%d, success=%d",
           has_device, strcmp(busid, "1-1") == 0, success);

  this->send_import_response_(success);

  if (success) {
    // Fresh pipes for this session: a previous attach that ended abruptly can
    // leave interfaces claimed with stale data toggles. Kill anything still in
    // flight, then release and re-claim so every pipe starts at DATA0 — the
    // client's own SET_CONFIGURATION during attach will do it again anyway.
    if (usb_client_hdl && usb_device_hdl) {
      this->quiesce_all_(true);
      this->release_all_interfaces_();
      this->claim_all_interfaces_();
    }
    this->client_state_ = CLIENT_STATE_ATTACHED;
    DEBUG_USB("Device attached to client, waiting for URBs");
  } else {
    DEBUG_USB("Import failed: device=%d, busid='%s'", has_device, busid);
  }

  return true;
}

void USBIPComponent::send_import_response_(bool success) {
  DEBUG_USB("Sending IMPORT response: success=%d", success);

  USBIPHeader header;
  header.version = htons_(this->negotiated_version_);
  header.command = htons_(OP_REP_IMPORT);
  header.status = htonl_(success ? ST_OK : ST_NA);

  DEBUG_USB("IMPORT header: version=0x%04X cmd=0x%04X status=0x%08lX",
           ntohs_(header.version), ntohs_(header.command), (unsigned long)ntohl_(header.status));

  if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&header), sizeof(header))) {
    DEBUG_USB("Failed to send IMPORT header");
    return;
  }

  if (success) {
    const USBDevice &d = this->usb_device_;
    USBIPDeviceInfo dev_info;
    fill_device_record(dev_info, d, htons_, htonl_);

    DEBUG_USB("IMPORT device: VID=%04X PID=%04X speed=%lu devnum=%d config=%d interfaces=%d",
             d.vid, d.pid, (unsigned long)d.speed, d.dev_addr,
             d.current_configuration, dev_info.bNumInterfaces);

    if (!this->write_bytes_(reinterpret_cast<uint8_t *>(&dev_info), sizeof(dev_info))) {
      DEBUG_USB("Failed to send IMPORT device info");
      return;
    }

    DEBUG_USB("IMPORT response sent (%zu + %zu bytes)",
             sizeof(header), sizeof(dev_info));
  } else {
    DEBUG_USB("IMPORT response sent (failure)");
  }
}

bool USBIPComponent::handle_urb_() {
  // URB header format (48 bytes total):
  // command(4) + seqnum(4) + devid(4) + direction(4) + ep(4) +
  // transfer_flags(4) + transfer_buffer_length(4) + start_frame(4) +
  // number_of_packets(4) + interval(4) + setup[8]
  uint8_t header[48];
  if (!this->read_bytes_(header, sizeof(header), 10000)) {
    // A partial header can't be re-synchronised: drop the client.
    DEBUG_USB("Failed to read URB header — dropping client");
    this->disconnect_client_();
    return false;
  }

  uint32_t command = ntohl_(*reinterpret_cast<uint32_t *>(&header[0]));
  uint32_t seqnum = ntohl_(*reinterpret_cast<uint32_t *>(&header[4]));
  uint32_t devid = ntohl_(*reinterpret_cast<uint32_t *>(&header[8]));
  uint32_t direction = ntohl_(*reinterpret_cast<uint32_t *>(&header[12]));
  uint32_t ep = ntohl_(*reinterpret_cast<uint32_t *>(&header[16]));
  uint32_t transfer_flags = ntohl_(*reinterpret_cast<uint32_t *>(&header[20]));
  uint32_t transfer_buffer_length = ntohl_(*reinterpret_cast<uint32_t *>(&header[24]));
  uint32_t number_of_packets = ntohl_(*reinterpret_cast<uint32_t *>(&header[32]));
  uint8_t *setup = &header[40];
  (void)devid;

  // Handle UNLINK command
  if (command == USBIP_CMD_UNLINK) {
    // For UNLINK the field at offset 20 holds the seqnum to cancel. Per spec a
    // successfully unlinked URB gets NO RET_SUBMIT — only this RET_UNLINK.
    uint32_t victim = transfer_flags;
    PendingUrb *p = find_urb_by_seqnum(victim);
    bool found = (p != nullptr);
    if (p) {
      p->unlinked = true;
      cancel_pending_urb(p, usb_device_hdl);
      if (p->done)
        free_urb_slot(p);
      // else: uncancellable ep0 URB — poll loop reaps it silently when done.
      // Neighbours on the same pipe were canceled too: put them back in order.
      resubmit_canceled_urbs();
    }
    TRACE_URB("UNLINK seqnum %lu %s", (unsigned long)victim,
              found ? "canceled" : "not pending");
    USBIPRetUnlink response;
    memset(&response, 0, sizeof(response));
    response.command = htonl_(USBIP_RET_UNLINK);
    response.seqnum = htonl_(seqnum);
    // Per protocol: -ECONNRESET when the URB was still pending and got unlinked,
    // 0 when it had already completed (RET_SUBMIT was/will be sent).
    response.status = htonl_(found ? (uint32_t)-LINUX_ECONNRESET : 0);
    this->write_bytes_(reinterpret_cast<uint8_t *>(&response), sizeof(response));
    return true;
  }

  if (command != USBIP_CMD_SUBMIT) {
    DEBUG_USB("Unknown URB command: 0x%04lX — dropping client", (unsigned long)command);
    this->disconnect_client_();
    return false;
  }

  // Endpoint transfer type from the parsed config descriptor (for logging and
  // ISO rejection). bmAttributes bits 1:0 — 0 ctrl, 1 iso, 2 bulk, 3 interrupt.
  uint8_t ep_attr = EP_TYPE_BULK;
  if (ep != 0) {
    const EndpointInfo *ei = this->usb_device_.find_endpoint(
        (uint8_t)((ep & 0x0F) | (direction ? 0x80 : 0x00)));
    if (ei) ep_attr = ei->attributes & 0x03;
  }
#if USBIP_TRACE_URBS
  static const char *const kEpTypes[] = {"ctrl", "iso", "bulk", "intr"};
  if (ep == 0)
    TRACE_URB("URB: seq=%lu ep0 ctrl %s len=%lu setup=%02X %02X %02X%02X %02X%02X %02X%02X",
              (unsigned long)seqnum, direction ? "IN" : "OUT",
              (unsigned long)transfer_buffer_length, setup[0], setup[1], setup[3], setup[2],
              setup[5], setup[4], setup[7], setup[6]);
  else
    TRACE_URB("URB: seq=%lu ep%lu %s %s len=%lu flags=%lx",
              (unsigned long)seqnum, (unsigned long)ep, kEpTypes[ep_attr],
              direction ? "IN" : "OUT", (unsigned long)transfer_buffer_length,
              (unsigned long)transfer_flags);
#endif

  // Read the OUT payload. Small payloads are consumed here in one go; large
  // ones (> one DMA chunk) stay in the socket and are streamed into the USB
  // transfer chunk by chunk, so print jobs / firmware uploads of any size go
  // through without a full-size heap copy.
  std::vector<uint8_t> buffer;
  bool stream_out = false;
  if (direction == 0 && transfer_buffer_length > 0) {
    if (ep != 0 && transfer_buffer_length > kMaxTransferSize &&
        transfer_buffer_length <= kMaxUrbOutSize) {
      stream_out = true;
    } else if (transfer_buffer_length > kMaxUrbOutSize) {
      DEBUG_USB("OUT URB seq=%lu of %lu bytes exceeds sanity cap — dropping client",
               (unsigned long)seqnum, (unsigned long)transfer_buffer_length);
      this->disconnect_client_();
      return false;
    } else {
      buffer.resize(transfer_buffer_length);
      if (!this->read_bytes_(buffer.data(), transfer_buffer_length, 10000)) {
        DEBUG_USB("Failed to read transfer buffer — dropping client");
        this->disconnect_client_();
        return false;
      }
    }
  }

  // Isochronous is not supported on this bridge (FS host + WiFi latency).
  // Per protocol, number_of_packets is 0xFFFFFFFF for non-ISO transfers.
  if (ep != 0 && (ep_attr == EP_TYPE_ISO ||
                  (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF))) {
    DEBUG_USB("ISO URB rejected (ep%lu, %lu packets)",
             (unsigned long)ep, (unsigned long)number_of_packets);
    if (stream_out) {  // consume the payload so the stream stays in sync
      uint8_t sink[512];
      uint32_t left = transfer_buffer_length;
      while (left) {
        size_t n = left > sizeof(sink) ? sizeof(sink) : left;
        if (!this->read_bytes_(sink, n, 10000)) { this->disconnect_client_(); return false; }
        left -= n;
      }
    }
    this->send_urb_response_(seqnum, -LINUX_EINVAL, nullptr, 0);
    return true;
  }

  return this->handle_cmd_submit_(seqnum, ep, direction, transfer_flags,
                                  transfer_buffer_length, setup, buffer.data(),
                                  stream_out);
}

// Send a no-data control request synchronously (CLEAR_FEATURE forwarding).
// Blocks until the device acks (client-event task delivers the callback) or 1s;
// on timeout the transfer is orphaned — never free an in-flight URB.
esp_err_t USBIPComponent::sync_control_out_(const uint8_t *setup) {
  if (!usb_device_hdl) return ESP_ERR_INVALID_STATE;
  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(64, 0, &xfer);
  if (err != ESP_OK) return err;
  memcpy(xfer->data_buffer, setup, 8);
  xfer->data_buffer[6] = 0;  // no data stage, whatever the caller's wLength says
  xfer->data_buffer[7] = 0;
  xfer->device_handle = usb_device_hdl;
  xfer->bEndpointAddress = 0;
  xfer->callback = sync_ctrl_cb;
  xfer->context = sync_ctrl_sem;
  xfer->num_bytes = 8;
  if (!s_ctrl_mtx) s_ctrl_mtx = xSemaphoreCreateMutex();
  if (s_ctrl_mtx) xSemaphoreTake(s_ctrl_mtx, portMAX_DELAY);
  xSemaphoreTake(sync_ctrl_sem, 0);  // drain any stale signal
  err = submit_control_retry(xfer);
  if (err != ESP_OK) {
    if (s_ctrl_mtx) xSemaphoreGive(s_ctrl_mtx);
    usb_host_transfer_free(xfer);
    return err;
  }
  bool done = xSemaphoreTake(sync_ctrl_sem, pdMS_TO_TICKS(1000)) == pdTRUE;
  if (s_ctrl_mtx) xSemaphoreGive(s_ctrl_mtx);
  if (!done) {
    DEBUG_USB("sync control timeout — orphaning transfer");
    return ESP_ERR_TIMEOUT;
  }
  usb_transfer_status_t st = xfer->status;
  usb_host_transfer_free(xfer);
  if (st == USB_TRANSFER_STATUS_COMPLETED) return ESP_OK;
  return st == USB_TRANSFER_STATUS_STALL ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
}

// --- ECU handshake replay --------------------------------------------------
// Captured session-start sequence (see usbip_bridge.h for provenance). Each
// step is a vendor control transfer: bmRequestType 0x40 = host writes 8 bytes
// to the ECU, 0xC0 = host reads 8 bytes back. wValue/wIndex behave like an
// address (bank/offset), not a standard USB field — the ECU's own scheme, not
// ours. Every byte here came out of two identical passes of a real capture.
struct HandshakeStep {
  uint8_t bmRequestType, bRequest;
  uint16_t wValue, wIndex, wLength;
  uint8_t data[8];  // only sent when bmRequestType's direction bit is OUT
};
static const HandshakeStep kHandshake[] = {
    {0xC0, 0xE0, 0x0001, 0x0000, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0001, 0x0005, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0001, 0x0141, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE4, 0x0001, 0x0100, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0x40, 0xF2, 0x0002, 0x0040, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0002, 0x0040, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0x40, 0xF2, 0x0002, 0x0140, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0002, 0x0140, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0x40, 0xF2, 0x0003, 0x0040, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0003, 0x0040, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0x40, 0xF2, 0x0003, 0x0140, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0xC0, 0xE2, 0x0003, 0x0140, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
};

// General synchronous control transfer (unlike sync_control_out_, this one
// carries a real data stage both ways and hands everything to the ecu tap so
// a capture of the replay itself is possible). Same blocking/timeout contract
// as sync_control_out_: the client-event daemon task delivers the callback.
static esp_err_t sync_control_xfer(uint8_t bmRequestType, uint8_t bRequest,
                                    uint16_t wValue, uint16_t wIndex,
                                    uint16_t wLength, uint8_t *inout_data) {
  if (!usb_device_hdl) return ESP_ERR_INVALID_STATE;
  if (!s_replay_sem) s_replay_sem = xSemaphoreCreateBinary();
  if (!s_ctrl_mtx) s_ctrl_mtx = xSemaphoreCreateMutex();
  if (!s_replay_sem || !s_ctrl_mtx) return ESP_ERR_NO_MEM;
  bool dev_to_host = (bmRequestType & 0x80) != 0;

  // A control IN's data stage must be a whole number of ep0 packets: the DWC
  // DMA is programmed in bMaxPacketSize0 units and writes a full packet into
  // the buffer even when the device answers fewer bytes. 8 + wLength (16) for
  // an 8-byte vendor read with a 64-byte ep0 overran the heap — the "Launch
  // Gauges" crash. Same rule the bridge applies to forwarded URBs.
  usb_device_info_t info{};
  uint16_t mps0 = (usb_host_device_info(usb_device_hdl, &info) == ESP_OK && info.bMaxPacketSize0)
                      ? info.bMaxPacketSize0 : 64;
  size_t data_len = dev_to_host ? ((size_t)wLength + mps0 - 1) / mps0 * mps0 : wLength;

  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(8 + data_len, 0, &xfer);
  if (err != ESP_OK) return err;
  uint8_t setup[8] = {bmRequestType, bRequest,
                      (uint8_t)(wValue & 0xFF), (uint8_t)(wValue >> 8),
                      (uint8_t)(wIndex & 0xFF), (uint8_t)(wIndex >> 8),
                      (uint8_t)(wLength & 0xFF), (uint8_t)(wLength >> 8)};
  memcpy(xfer->data_buffer, setup, 8);
  if (!dev_to_host && wLength > 0 && inout_data)
    memcpy(xfer->data_buffer + 8, inout_data, wLength);
  xfer->device_handle = usb_device_hdl;
  xfer->bEndpointAddress = 0;
  xfer->callback = sync_ctrl_cb;
  xfer->context = s_replay_sem;
  xfer->num_bytes = 8 + data_len;
  ecu::on_control(0, setup, (!dev_to_host && wLength) ? inout_data : nullptr,
                  !dev_to_host ? wLength : 0);

  xSemaphoreTake(s_ctrl_mtx, portMAX_DELAY);
  xSemaphoreTake(s_replay_sem, 0);  // drain any stale signal
  err = submit_control_retry(xfer);
  if (err != ESP_OK) {
    xSemaphoreGive(s_ctrl_mtx);
    usb_host_transfer_free(xfer);
    return err;
  }
  bool done = xSemaphoreTake(s_replay_sem, pdMS_TO_TICKS(1000)) == pdTRUE;
  xSemaphoreGive(s_ctrl_mtx);
  if (!done) {
    DEBUG_USB("handshake replay: step timed out");
    return ESP_ERR_TIMEOUT;  // in flight: never free a queued transfer
  }
  usb_transfer_status_t st = xfer->status;
  if (st == USB_TRANSFER_STATUS_COMPLETED && dev_to_host && wLength > 0 && inout_data) {
    int got = xfer->actual_num_bytes - 8;
    if (got < 0) got = 0;
    if (got > wLength) got = wLength;
    memcpy(inout_data, xfer->data_buffer + 8, got);
    ecu::on_control(1, setup, inout_data, got);
  }
  usb_host_transfer_free(xfer);
  if (st == USB_TRANSFER_STATUS_COMPLETED) return ESP_OK;
  return st == USB_TRANSFER_STATUS_STALL ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
}

static char s_launch_err[96] = "";
const char *last_launch_error() { return s_launch_err; }

bool replay_ecu_handshake() {
  s_launch_err[0] = 0;
  // A mux toggle (waking from sleep, or USB-C/laptop handoff) disconnects and
  // re-enumerates the ECU; give that a couple of seconds before giving up,
  // rather than failing immediately on a device that is mid-reconnect.
  for (int waited_ms = 0; waited_ms < 2000; waited_ms += 50) {
    if (usb_device_hdl && g_usbip_component && g_usbip_component->device_info().ready) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!usb_device_hdl) {
    snprintf(s_launch_err, sizeof(s_launch_err), "no ECU on USB-A (nothing enumerated)");
    DEBUG_USB("handshake replay: %s", s_launch_err);
    return false;
  }
  if (g_usbip_component && !g_usbip_component->device_info().ready) {
    snprintf(s_launch_err, sizeof(s_launch_err), "ECU present but not enumerated yet");
    DEBUG_USB("handshake replay: %s", s_launch_err);
    return false;
  }
  if (g_usbip_component && g_usbip_component->client_attached()) {
    snprintf(s_launch_err, sizeof(s_launch_err),
             "a USB/IP client (PC) is attached - detach it first, or just use Ignitron");
    DEBUG_USB("handshake replay: %s", s_launch_err);
    return false;
  }
  for (auto &step : kHandshake) {
    uint8_t buf[8];
    memcpy(buf, step.data, sizeof(buf));
    esp_err_t err = sync_control_xfer(step.bmRequestType, step.bRequest, step.wValue,
                                       step.wIndex, step.wLength, buf);
    if (err != ESP_OK) {
      snprintf(s_launch_err, sizeof(s_launch_err),
               "step %u bReq=%02X wValue=%04X wIndex=%04X: %s",
               (unsigned)(&step - kHandshake) + 1, step.bRequest, step.wValue, step.wIndex,
               err == ESP_ERR_TIMEOUT ? "no reply (timeout)" :
               err == ESP_ERR_NOT_SUPPORTED ? "ECU stalled it" : esp_err_to_name(err));
      DEBUG_USB("handshake replay: %s", s_launch_err);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  DEBUG_USB("handshake replay: completed %u steps",
           (unsigned)(sizeof(kHandshake) / sizeof(kHandshake[0])));
  return true;
}

// --- Standalone gauge poll --------------------------------------------------
// The handshake above is the session-start identity block (E0/E2/E4 reads of
// bank 1, then F2 "select address" + E2 "read 8 bytes" at bank 2/3 words
// 0x40 and 0x140 - firmware/serial fields the PC decodes and displays). It
// never asks for gauge data; the live data only arrives in answer to the FA
// poll below, one block per command. Both the identity replies and the tune
// (F9) reads are XOR-obfuscated with a per-bank keystream that
// tools/ignitron_tune.py reproduces; the live-data (FA) blocks are plain.
// Each reply is fed straight into ecu::on_bulk_in() -> Decoder, the same
// path that decodes a real usbip client's traffic.
//
// Deliberately its own separate transfer path (not the PendingUrb table) —
// same reasoning as sync_control_xfer: this runs with no usbip client
// attached at all, so there is nothing to coordinate with, and reusing the
// client-driven machinery would be solving a problem that doesn't exist here.
static SemaphoreHandle_t s_bulk_sem = nullptr;
static void bulk_poll_cb(usb_transfer_t *t) {
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
}

static esp_err_t sync_bulk_out(uint8_t ep_addr, const uint8_t *data, size_t len) {
  if (!usb_device_hdl) return ESP_ERR_INVALID_STATE;
  if (!s_bulk_sem) s_bulk_sem = xSemaphoreCreateBinary();
  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(len, 0, &xfer);
  if (err != ESP_OK) return err;
  memcpy(xfer->data_buffer, data, len);
  xfer->device_handle = usb_device_hdl;
  xfer->bEndpointAddress = ep_addr;
  xfer->callback = bulk_poll_cb;
  xfer->context = s_bulk_sem;
  xfer->num_bytes = len;
  xfer->timeout_ms = 0;
  xSemaphoreTake(s_bulk_sem, 0);
  err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    usb_host_transfer_free(xfer);
    return err;
  }
  bool done = xSemaphoreTake(s_bulk_sem, pdMS_TO_TICKS(500)) == pdTRUE;
  esp_err_t result = ESP_ERR_TIMEOUT;
  if (done) result = xfer->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
  if (done) usb_host_transfer_free(xfer);  // in flight: never free a queued transfer
  return result;
}

static esp_err_t sync_bulk_in(uint8_t ep_addr, uint8_t *buf, size_t cap, size_t *out_actual,
                              uint32_t timeout_ms = 500) {
  if (!usb_device_hdl) return ESP_ERR_INVALID_STATE;
  if (!s_bulk_sem) s_bulk_sem = xSemaphoreCreateBinary();
  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(cap, 0, &xfer);
  if (err != ESP_OK) return err;
  xfer->device_handle = usb_device_hdl;
  xfer->bEndpointAddress = ep_addr;
  xfer->callback = bulk_poll_cb;
  xfer->context = s_bulk_sem;
  xfer->num_bytes = cap;
  xfer->timeout_ms = 0;
  xSemaphoreTake(s_bulk_sem, 0);
  err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    usb_host_transfer_free(xfer);
    return err;
  }
  bool done = xSemaphoreTake(s_bulk_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  if (!done) return ESP_ERR_TIMEOUT;  // in flight: never free a queued transfer
  esp_err_t result = xfer->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
  if (result == ESP_OK && out_actual) {
    size_t got = xfer->actual_num_bytes;
    if (got > cap) got = cap;
    memcpy(buf, xfer->data_buffer, got);
    *out_actual = got;
  }
  usb_host_transfer_free(xfer);
  return result;
}

static volatile bool s_gauge_poll_active = false;
static TaskHandle_t s_gauge_poll_task_hdl = nullptr;

// One vendor control OUT per poll, then the reply on bulk-IN ep 0x81. The
// command's data[0] is the reply length in 8-byte words, header included:
//   FA wValue=3 wIndex=0x800 data 0x59  -> 8 B header + 704 B (channels 32..383)
//   FA wValue=2 wIndex=0x800 data 0x09  -> 8 B header +  64 B (channels  0..31)
// What the wire actually carries (tools/launch_monitor.py, launch.bin,
// 2026-09-15): the ECU streams the reply in whole 64-byte packets - the 8
// header bytes and the first 56 bytes of the block share the first packet,
// and the tail is padded to a packet boundary with whatever follows in
// memory. 712 -> 768 bytes and 72 -> 128 bytes. So the reply is read as ONE
// transfer of the padded size and the block is taken from byte 8. Reading
// "8 bytes, then the block" the way Ignitron.exe does only works when the
// host controller happens to drop the rest of that first packet; on ESP-IDF
// it hands over the whole packet, which is how the previous version ended up
// feeding the decoder a block shifted by 56 bytes. If the ECU ever does send
// the header as a lone short packet, the transfer completes with 8 bytes and
// the block is fetched with a second read. The PC alternates the two polls
// every ~40 ms and issues the next command ~5 ms after the block lands.
static uint8_t s_poll_buf[1152];  // largest padded reply: 8 + 1024 (CPU fault table) -> 17 packets
// One transaction on the ECU at a time: the poll task and the fault-memory
// operations (web task) both take this around their command + reply.
static SemaphoreHandle_t s_ecu_mtx = nullptr;
static bool ecu_lock_(uint32_t ms) {
  if (!s_ecu_mtx) s_ecu_mtx = xSemaphoreCreateMutex();
  return s_ecu_mtx && xSemaphoreTake(s_ecu_mtx, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void ecu_unlock_() { if (s_ecu_mtx) xSemaphoreGive(s_ecu_mtx); }

// Send a read command (bRequest 0xFA live data / 0xFB memory, data[0] = reply
// length in 8-byte words, header word included) and collect the padded reply.
// On success *out points at the payload (the word after the header) inside
// s_poll_buf and hdr at the 8-byte header word. Caller holds the ECU lock.
static esp_err_t ecu_read_(uint8_t bReq, uint16_t bank, uint16_t word, size_t payload_len,
                           const uint8_t **out, const uint8_t **hdr) {
  const size_t reply_len = 8 + payload_len;
  if (reply_len > sizeof(s_poll_buf)) return ESP_ERR_INVALID_SIZE;
  uint8_t cmd[8] = {(uint8_t)(reply_len / 8), 0, 0, 0, 0, 0, 0, 0};
  esp_err_t err = sync_control_xfer(0x40, bReq, bank, word, 8, cmd);
  if (err != ESP_OK) return err;
  const size_t padded = (reply_len + 63) / 64 * 64;
  size_t got = 0;
  err = sync_bulk_in(0x81, s_poll_buf, padded, &got, 300);
  if (err != ESP_OK) return err;
  if (got == 8) {   // header as a lone short packet, block follows
    static uint8_t hdr8[8];
    memcpy(hdr8, s_poll_buf, 8);
    got = 0;
    err = sync_bulk_in(0x81, s_poll_buf + 8, padded - 8, &got, 300);
    if (err != ESP_OK) return err;
    if (got < payload_len) return ESP_ERR_INVALID_RESPONSE;
    memcpy(s_poll_buf, hdr8, 8);
  } else if (got < reply_len) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (hdr) *hdr = s_poll_buf;
  if (out) *out = s_poll_buf + 8;
  return ESP_OK;
}

// Write `len` bytes to memory: the command (wValue = bank | 0x80) then the data
// as one bulk OUT, exactly the pair Ignitron.exe sends. Caller holds the lock.
static esp_err_t ecu_write_(uint16_t bank, uint16_t word, const uint8_t *data, size_t len) {
  uint8_t zeros[8] = {0};
  esp_err_t err = sync_control_xfer(0x40, 0xFB, (uint16_t)(bank | 0x80), word, 8, zeros);
  if (err != ESP_OK) return err;
  err = sync_bulk_out(0x01, data, len);
  if (err == ESP_OK) ecu::on_bulk_out(0x01, data, len);
  return err;
}

// Read and discard whatever the ECU still has queued on ep 0x81 (an earlier
// reply nobody collected), so the first real poll starts on a packet boundary.
static void drain_ecu_replies_() {
  for (int i = 0; i < 8; i++) {
    size_t got = 0;
    if (sync_bulk_in(0x81, s_poll_buf, sizeof(s_poll_buf), &got, 40) != ESP_OK || got == 0) break;
    DEBUG_USB("gauge poll: drained %u stale bytes", (unsigned)got);
  }
}

static void gauge_poll_task(void *) {
  bool dsp_block = false;       // false: 704 B main block, true: 64 B DSP block
  uint32_t fail_log_ms = 0;
  for (;;) {
    if (!s_gauge_poll_active || !usb_device_hdl ||
        (g_usbip_component && g_usbip_component->client_attached())) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    const uint16_t wValue = dsp_block ? 0x0002 : 0x0003;
    const size_t block_len = dsp_block ? 64 : 704;
    dsp_block = !dsp_block;
    if (!ecu_lock_(200)) continue;            // a fault-memory operation has the bus
    const uint8_t *block = nullptr;
    esp_err_t err = ecu_read_(0xFA, wValue, 0x0800, block_len, &block, nullptr);
    if (err == ESP_OK) {
      ecu::on_bulk_in(0x81 & 0x0F, block, block_len);
      ecu_unlock_();
      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      // Nothing usable: log (rate-limited), resync, and try again shortly.
      uint32_t now = millis();
      if (now - fail_log_ms > 2000) {
        fail_log_ms = now;
        DEBUG_USB("gauge poll: %s reply %s", dsp_block ? "main" : "DSP", esp_err_to_name(err));
      }
      drain_ecu_replies_();
      ecu_unlock_();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// --- ECU fault memory ---------------------------------------------------------
static bool ecu_available_(const char *what) {
  s_launch_err[0] = 0;
  if (!usb_device_hdl || !(g_usbip_component && g_usbip_component->device_info().ready)) {
    snprintf(s_launch_err, sizeof(s_launch_err), "%s: no ECU enumerated on USB-A", what);
    return false;
  }
  if (g_usbip_component && g_usbip_component->client_attached()) {
    snprintf(s_launch_err, sizeof(s_launch_err), "%s: a USB/IP client (PC) owns the ECU", what);
    return false;
  }
  return true;
}

// Parse one table (512 or 1024 B) of 32-byte pages into `out`.
static bool parse_fault_table_(const uint8_t *tbl, size_t len, uint8_t src, EcuFaultMemory &out) {
  bool all_ok = true;
  for (size_t pg = 0; pg + 32 <= len; pg += 32) {
    const uint8_t *p = tbl + pg;
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) sum += p[i];
    uint16_t stored = (uint16_t)(p[30] | (p[31] << 8));
    bool empty_page = (sum == 0x1DE2);          // 30 x 0xFF: Ignitron treats it as blank
    bool page_ok = empty_page || sum == stored;
    if (!page_ok) all_ok = false;
    for (int r = 0; r < 3; r++) {
      const uint8_t *q = p + r * 10;
      uint16_t w[5];
      for (int k = 0; k < 5; k++) w[k] = (uint16_t)(q[2 * k] | (q[2 * k + 1] << 8));
      if (w[0] == 0xFFFF) continue;
      if (out.count >= sizeof(out.faults) / sizeof(out.faults[0])) break;
      EcuFault &f = out.faults[out.count++];
      f.code = w[0]; f.rpm = w[1]; f.load = w[2]; f.value = w[3];
      f.count = (uint8_t)((w[4] >> 12) + 1);
      f.duration = (uint16_t)(w[4] & 0x0FFF);
      f.src = src;
      f.page_ok = page_ok;
    }
  }
  return all_ok;
}

bool ecu_read_faults(EcuFaultMemory &out) {
  memset(&out, 0, sizeof(out));
  if (!ecu_available_("fault read")) return false;
  if (!ecu_lock_(1500)) {
    snprintf(s_launch_err, sizeof(s_launch_err), "fault read: ECU bus busy");
    return false;
  }
  const uint8_t *data = nullptr, *hdr = nullptr;
  // DSP: FB bank 2 word 0x1F8, 0x42 words -> counter word + 512 B table at 0x200
  esp_err_t err = ecu_read_(0xFB, 2, 0x01F8, 512, &data, &hdr);
  if (err == ESP_OK) {
    out.dsp_ok = true;
    out.dsp_counter = (uint16_t)(hdr[6] | (hdr[7] << 8));
    out.dsp_csum = parse_fault_table_(data, 512, 1, out);
  }
  // CPU: FB bank 3 word 0xBF8, 0x82 words -> counter word + 1024 B table at 0xC00
  err = ecu_read_(0xFB, 3, 0x0BF8, 1024, &data, &hdr);
  if (err == ESP_OK) {
    out.cpu_ok = true;
    out.cpu_counter = (uint16_t)(hdr[6] | (hdr[7] << 8));
    out.cpu_csum = parse_fault_table_(data, 1024, 2, out);
  }
  ecu_unlock_();
  if (!out.dsp_ok || !out.cpu_ok) {
    snprintf(s_launch_err, sizeof(s_launch_err), "fault read: %s table %s",
             out.dsp_ok ? "CPU" : "DSP", esp_err_to_name(err));
    return false;
  }
  return true;
}

// The clear, exactly as captured from Ignitron (faults1.bin, 2026-09-16):
//   FB write bank 2 word 0x200  512 x 0xFF, read back 0x41 words and compare
//   FB write bank 3 word 0xC00 1024 x 0xFF, read back 0x81 words and compare
//   then per bank a commit: F3 (write 8 B "00 00 AA 55 00 00 00 00" at 0x960 /
//   0xAE0), F4 (read-word command at the same address), E4 (fetch the 8 B)
//   and check they read back as written.
bool ecu_clear_faults() {
  if (!ecu_available_("fault clear")) return false;
  if (!ecu_lock_(3000)) {
    snprintf(s_launch_err, sizeof(s_launch_err), "fault clear: ECU bus busy");
    return false;
  }
  static uint8_t ff[1024];
  memset(ff, 0xFF, sizeof(ff));
  bool ok = true;
  const uint8_t *data = nullptr;
  struct { uint16_t bank, word; size_t len; } tables[2] = {{2, 0x0200, 512}, {3, 0x0C00, 1024}};
  for (auto &t : tables) {
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && ok; attempt++) {
      err = ecu_write_(t.bank, t.word, ff, t.len);
      if (err != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
      vTaskDelay(pdMS_TO_TICKS(60));
      err = ecu_read_(0xFB, t.bank, t.word, t.len, &data, nullptr);
      if (err == ESP_OK && memcmp(data, ff, t.len) == 0) break;
      err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
      ok = false;
      snprintf(s_launch_err, sizeof(s_launch_err), "fault clear: %s table not cleared (%s)",
               t.bank == 2 ? "DSP" : "CPU", esp_err_to_name(err));
    }
  }
  struct { uint16_t bank, word; } commits[2] = {{2, 0x0960}, {3, 0x0AE0}};
  for (auto &c : commits) {
    if (!ok) break;
    uint8_t magic[8] = {0x00, 0x00, 0xAA, 0x55, 0, 0, 0, 0};
    uint8_t rd_cmd[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t back[8] = {0};
    esp_err_t err = sync_control_xfer(0x40, 0xF3, c.bank, c.word, 8, magic);
    if (err == ESP_OK) err = sync_control_xfer(0x40, 0xF4, c.bank, c.word, 8, rd_cmd);
    if (err == ESP_OK) err = sync_control_xfer(0xC0, 0xE4, c.bank, 0x0000, 8, back);
    if (err != ESP_OK || memcmp(back, magic, 4) != 0) {
      ok = false;
      snprintf(s_launch_err, sizeof(s_launch_err), "fault clear: %s commit %s",
               c.bank == 2 ? "DSP" : "CPU", err == ESP_OK ? "not acknowledged" : esp_err_to_name(err));
    }
  }
  ecu_unlock_();
  return ok;
}

void set_gauge_poll(bool on) {
  if (on == s_gauge_poll_active) return;
  if (on) {
    if (!s_gauge_poll_task_hdl)
      xTaskCreatePinnedToCore(gauge_poll_task, "gauge_poll", 4096, nullptr, 2,
                              &s_gauge_poll_task_hdl, 0);
    // The one bulk-OUT the PC ever sends: 16 zero bytes written to bank 3
    // word 0xAE8 (the host->ECU mailbox that "realtime override" later fills
    // with a 0xDEAD-tagged record). It is the data stage of an FA *write*
    // command - wValue 0x0083 = bank 3 | 0x80 - so send that first; a bare
    // OUT with no command in front of it is not something the ECU ever sees
    // from Ignitron.exe (which retries the pair until the OUT returns 16).
    uint8_t zeros[8] = {0};
    esp_err_t err = sync_control_xfer(0x40, 0xFA, 0x0083, 0x0AE8, 8, zeros);
    uint8_t poke[16] = {0};
    if (err == ESP_OK) {
      err = sync_bulk_out(0x01, poke, sizeof(poke));
      ecu::on_bulk_out(0x01, poke, sizeof(poke));   // show it in captures too
    }
    DEBUG_USB("gauge poll: start (mailbox clear %s)", esp_err_to_name(err));
    drain_ecu_replies_();   // a reply left over from an earlier session would misframe the first poll
  } else {
    DEBUG_USB("gauge poll: stop");
  }
  s_gauge_poll_active = on;
}

bool gauge_poll_active() { return s_gauge_poll_active; }

// --- Modem-line (DTR/RTS) coalescing --------------------------------------------
// Auto-reset circuits on ESP32/AVR boards decode DTR+RTS *together*: the
// bootloader entry sequence esptool sends is DTR=1,RTS=1 → DTR=1,RTS=0 as two
// consecutive line changes, and the intermediate state releases EN with IO0
// still high. Locally the two are ~1 ms apart and the RC on EN hides it; over
// this bridge each is its own control URB with a full WiFi round trip, so the
// target boots normally before IO0 drops ("wrong boot mode detected"). Same
// race for any host-side logic that drives two lines "at once".
//
// Remedy (equivalent to esptool's Unix TIOCMSET "tight reset"): acknowledge
// line-state requests to the client immediately and apply them to the adapter
// after a short hold-off, merging whatever arrives meanwhile so the adapter
// only sees the final state. Ordering is preserved: the pending state is
// flushed before any other URB reaches the device. The requests recognised
// are the 0-length vendor/class OUTs of the common adapters:
//   CH340/CH341  0x40 / 0xA4 MODEM_CTRL           wValue = ~lines (whole word)
//   CP210x       0x41 / 0x07 SET_MHS              wValue = mask<<8 | lines
//   FTDI         0x40 / 0x01 SIO_SET_MODEM_CTRL   wValue = mask<<8 | lines, wIndex = port
//   CDC-ACM/PL2303 0x21 / 0x22 SET_CONTROL_LINE_STATE wValue = lines, wIndex = iface

static LineStateKind line_state_kind(const uint8_t *setup) {
  uint8_t rt = setup[0], rq = setup[1];
  uint16_t wLength = setup[6] | (setup[7] << 8);
  if (wLength != 0) return LS_NONE;
  if (rt == 0x40 && rq == 0xA4) return LS_WHOLE;   // CH340 MODEM_CTRL
  if (rt == 0x21 && rq == 0x22) return LS_WHOLE;   // CDC SET_CONTROL_LINE_STATE
  if (rt == 0x41 && rq == 0x07) return LS_MASKED;  // CP210x SET_MHS
  if (rt == 0x40 && rq == 0x01) return LS_MASKED;  // FTDI SIO_SET_MODEM_CTRL
  return LS_NONE;
}

bool USBIPComponent::absorb_line_state_(uint32_t seqnum, const uint8_t *setup) {
  if (USBIP_LINE_STATE_HOLD_MS == 0) return false;
  LineStateKind kind = line_state_kind(setup);
  if (kind == LS_NONE) return false;

  LineState &ls = s_line_state;
  bool same_target = ls.pending && ls.setup[0] == setup[0] && ls.setup[1] == setup[1] &&
                     ls.setup[4] == setup[4] && ls.setup[5] == setup[5];
  if (ls.pending && !same_target) this->flush_line_state_(true);  // different request/port: keep order

  if (ls.pending && kind == LS_MASKED) {
    // Merge: lines named by the new mask take the new value, others keep the
    // pending value; the combined mask covers both.
    uint8_t mask = ls.setup[3] | setup[3];
    uint8_t lines = (ls.setup[2] & ~setup[3]) | (setup[2] & setup[3]);
    memcpy(ls.setup, setup, 8);
    ls.setup[2] = lines;
    ls.setup[3] = mask;
  } else {
    if (!ls.pending) ls.first_us = esp_timer_get_time();
    memcpy(ls.setup, setup, 8);
  }
  ls.pending = true;
  this->send_urb_response_(seqnum, 0, nullptr, 0);
  return true;
}

// Apply the pending line state to the adapter. force=false only flushes once
// the hold-off has elapsed (poll loop); force=true flushes now (something
// else is about to reach the device, or the session is ending).
void USBIPComponent::flush_line_state_(bool force) {
  LineState &ls = s_line_state;
  if (!ls.pending) return;
  if (!force && esp_timer_get_time() - ls.first_us < (int64_t)USBIP_LINE_STATE_HOLD_MS * 1000)
    return;
  ls.pending = false;
  if (!usb_device_hdl || !this->usb_device_.connected) return;
  esp_err_t err = this->sync_control_out_(ls.setup);
  if (err != ESP_OK)
    DEBUG_USB("Line-state %02X/%02X wValue=%04X: %s", ls.setup[0], ls.setup[1],
             ls.setup[2] | (ls.setup[3] << 8), esp_err_to_name(err));
}

// --- Intercepted standard requests --------------------------------------------
// The Linux usbip stub driver runs exactly these four through the host stack
// instead of forwarding them raw (stub_rx.c tweak_special_requests), because
// each resets endpoint state on the device that the host must mirror.

// Kill every in-flight URB, tear down every pipe, SET_CONFIGURATION(value) on
// the device (all toggles → DATA0, all interfaces → alt 0), re-claim.
esp_err_t USBIPComponent::reconfigure_device_(uint8_t value) {
  USBDevice &d = this->usb_device_;
  this->quiesce_all_(true);
  this->release_all_interfaces_();
  uint8_t setup[8] = {0x00, USB_REQ_SET_CONFIGURATION, value, 0x00, 0x00, 0x00, 0x00, 0x00};
  esp_err_t err = this->sync_control_out_(setup);
  if (value != 0) {
    d.configured = true;
    for (auto &a : d.iface_alt) a = 0;
    this->claim_all_interfaces_();
  } else {
    d.configured = false;
  }
  return err;
}

// SET_CONFIGURATION: the device drops all endpoint state (toggles → DATA0).
// Mirror it by tearing down and re-claiming every interface. ESP-IDF's usbh
// only knows the configuration it enumerated, so a *different* configuration
// value can't be honoured; the same value (what every driver sends on start
// or re-open — libusb-win32/WinUSB do it on every open) and 0 (unconfigure)
// are handled.
void USBIPComponent::handle_set_configuration_(uint32_t seqnum, const uint8_t *setup) {
  crumb::set(crumb::BRIDGE_INTERCEPT);
  USBDevice &d = this->usb_device_;
  uint8_t value = setup[2];
  if (value != 0 && value != d.current_configuration) {
    DEBUG_USB("SET_CONFIGURATION(%d) refused: only config %d is enumerated",
             value, d.current_configuration);
    this->send_urb_response_(seqnum, -LINUX_EPIPE, nullptr, 0);
    return;
  }
  esp_err_t err = this->reconfigure_device_(value);
  DEBUG_USB("SET_CONFIGURATION(%d): %s (pipes recreated)", value, esp_err_to_name(err));
  this->send_urb_response_(seqnum, err == ESP_OK ? 0 : -LINUX_EPIPE, nullptr, 0);
}

// SET_FEATURE(PORT_RESET) with bmRequestType 0x23 (class, recipient "other")
// is not a request for the device at all: it is how a USB/IP client asks the
// server to reset the device — usbip-win2 sends it from its port-reset
// callback whenever a Windows function driver issues
// IOCTL_INTERNAL_USB_RESET_PORT (FTDI's FT_ResetPort, ftdibus.sys' own
// error recovery, ...), and the Linux stub answers it with usb_reset_device()
// without ever forwarding it. Forwarded raw, a serial adapter STALLs the
// unknown hub-class request, the client sees -EPIPE, marks the reset as
// failed and the device is dead until re-attach. ESP-IDF has no per-device
// reset short of dropping port power (which the client would see as an
// unplug), so emulate what the reset ends in: every in-flight URB fails, the
// configuration is re-selected on the device (toggles and alternate settings
// back to defaults) and the pipes are rebuilt. The reply is 0, as the stub's.
void USBIPComponent::handle_reset_device_(uint32_t seqnum, const uint8_t *setup) {
  crumb::set(crumb::BRIDGE_INTERCEPT);
  (void)setup;
  USBDevice &d = this->usb_device_;
  s_line_state.pending = false;  // a reset drops whatever the driver had asked for
  esp_err_t err = this->reconfigure_device_(d.current_configuration);
  DEBUG_USB("Device reset (SET_FEATURE PORT_RESET): SET_CONFIGURATION(%d) %s, pipes recreated",
           d.current_configuration, esp_err_to_name(err));
  this->send_urb_response_(seqnum, err == ESP_OK ? 0 : -LINUX_EPIPE, nullptr, 0);
}

// SET_INTERFACE: switch an interface's alternate setting. URBs in flight on
// the old endpoints are failed with -ESHUTDOWN (Linux usb_set_interface
// disables them the same way), the interface is re-claimed with the new alt
// (ESP-IDF supports any bAlternateSetting in usb_host_interface_claim), and
// the endpoint table follows.
void USBIPComponent::handle_set_interface_(uint32_t seqnum, const uint8_t *setup) {
  crumb::set(crumb::BRIDGE_INTERCEPT);
  USBDevice &d = this->usb_device_;
  uint8_t alt = setup[2];
  uint8_t num = setup[4];
  int idx = d.iface_index(num);
  const InterfaceInfo *target = d.find_interface(num, alt);
  if (idx < 0 || !target) {
    DEBUG_USB("SET_INTERFACE(iface %d alt %d): no such alternate setting", num, alt);
    this->send_urb_response_(seqnum, -LINUX_EPIPE, nullptr, 0);
    return;
  }
  uint8_t prev_alt = d.iface_alt[idx];
  const InterfaceInfo *cur = d.active_interface(num);
  if (cur) this->quiesce_interface_(*cur, true);
  this->release_interface_(num);

  esp_err_t err = this->sync_control_out_(setup);
  // USB 2.0 §9.4.10: a device whose interface has a single alternate setting
  // may STALL SET_INTERFACE; Linux treats that as success and so do we.
  bool ok = (err == ESP_OK) ||
            (err == ESP_ERR_NOT_SUPPORTED && d.num_alt_settings(num) == 1);
  uint8_t new_alt = ok ? alt : prev_alt;
  this->claim_interface_(num, new_alt);
  DEBUG_USB("SET_INTERFACE(iface %d alt %d): %s -> active alt %d", num, alt,
           esp_err_to_name(err), new_alt);
  this->send_urb_response_(seqnum, ok ? 0 : -LINUX_EPIPE, nullptr, 0);
}

// CLEAR_FEATURE(ENDPOINT_HALT): the device un-stalls the endpoint and resets
// its toggle to DATA0. ESP-IDF's halt/flush/clear never touch the host pipe's
// PID (hcd_dwc.c: only channel allocation starts at DATA0), so without a
// pipe rebuild the host keeps expecting DATA1, ACKs-but-discards the next
// packet and the transfer hangs — the classic post-STALL mass-storage wedge,
// and the state every serial/vendor driver's "reset pipe" on open lands in.
// URBs the driver keeps parked on the interface's other endpoints (a serial
// adapter's always-pending bulk IN) are canceled around the rebuild and
// resubmitted in order, rather than blocking the rebuild.
void USBIPComponent::handle_clear_halt_(uint32_t seqnum, const uint8_t *setup) {
  crumb::set(crumb::BRIDGE_INTERCEPT);
  USBDevice &d = this->usb_device_;
  uint8_t ep = setup[4];
  int ifn = d.interface_of_endpoint(ep);
  int idx = ifn >= 0 ? d.iface_index((uint8_t)ifn) : -1;
  const InterfaceInfo *iface = ifn >= 0 ? d.active_interface((uint8_t)ifn) : nullptr;

  bool rebuild = iface && idx >= 0 && d.iface_claimed[idx];
  if (rebuild) this->quiesce_interface_(*iface, false);

  esp_err_t ferr = this->sync_control_out_(setup);

  bool rebuilt = false;
  if (rebuild) {
    // Recreating the interface's pipes restarts EVERY host toggle on it at
    // DATA0, so the device's other endpoints must restart too: CLEAR_FEATURE
    // (HALT) always reinitialises an endpoint's toggle, halted or not (USB 2.0
    // §9.4.5). Without this the device's OUT toggle stays at DATA1, it ACKs
    // but discards the next CBW, and mass-storage reads hang after recovery.
    for (auto &other : iface->endpoints) {
      if (other.address == ep || (other.attributes & 0x03) == EP_TYPE_ISO) continue;
      uint8_t clr[8] = {0x02, USB_REQ_CLEAR_FEATURE, 0x00, 0x00, other.address, 0x00, 0x00, 0x00};
      esp_err_t oerr = this->sync_control_out_(clr);
      if (oerr != ESP_OK)
        DEBUG_USB("Toggle-reset CLEAR_FEATURE on ep 0x%02X: %s", other.address, esp_err_to_name(oerr));
    }
    rebuilt = this->recreate_interface_pipes_((uint8_t)ifn);
  }
  if (!rebuilt && usb_device_hdl && (ep & 0x0F)) {
    // Unknown/unclaimed endpoint: at least un-halt the pipe if we have one.
    usb_host_endpoint_halt(usb_device_hdl, ep);
    usb_host_endpoint_flush(usb_device_hdl, ep);
    usb_host_endpoint_clear(usb_device_hdl, ep);
  }
  resubmit_canceled_urbs();
  DEBUG_USB("CLEAR_FEATURE(HALT) ep 0x%02X: device %s, pipes %s", ep,
           esp_err_to_name(ferr), rebuilt ? "recreated" : "cleared");
  this->send_urb_response_(seqnum, ferr == ESP_OK ? 0 : -LINUX_EPIPE, nullptr, 0);
}

// Prepare and submit the next chunk of a chunked URB (chunked_in or stream_out).
// For streamed OUT URBs the chunk is read straight from the TCP socket; a
// short/failed read drops the client (the stream can't be resynchronised).
esp_err_t USBIPComponent::submit_urb_chunk_(PendingUrb *p) {
  uint32_t remaining = p->buflen - p->accum_off;
  uint32_t cap = p->xfer->data_buffer_size;  // mps-aligned by allocation
  uint32_t nb;
  if (p->direction) {
    nb = ((remaining + p->mps - 1) / p->mps) * p->mps;
    if (nb > cap) nb = cap;
  } else {
    nb = remaining > cap ? cap : remaining;
    if (p->stream_out) {
      // Payload bytes arrive from the client in URB order; pull this chunk.
      if (!this->read_bytes_(p->xfer->data_buffer, nb, 10000))
        return ESP_ERR_TIMEOUT;
      if (p->accum_off + nb >= p->buflen && s_stream_urb == p)
        s_stream_urb = nullptr;  // whole payload consumed: stream is free again
    }
    if (is_tappable_ep_type(p->ep_type))
      ecu::on_bulk_out(p->ep_addr & 0x0F, p->xfer->data_buffer, nb);
    if (p->want_zlp && nb == remaining && (p->buflen % p->mps) == 0)
      p->xfer->flags |= USB_TRANSFER_FLAG_ZERO_PACK;  // only on the final chunk
    else
      p->xfer->flags &= ~USB_TRANSFER_FLAG_ZERO_PACK;
  }
  p->chunk_bytes = nb;
  p->xfer->num_bytes = nb;
  p->done = false;
  return usb_host_transfer_submit(p->xfer);
}

// CMD_SUBMIT → one freshly-allocated usb_transfer_t, submitted immediately and
// parked in the pending table; the reply is sent from poll_pending_urbs_() when
// its callback fires. Faithful port of the reference engines: control requests
// are forwarded to the device untouched (all GET_DESCRIPTORs, class and vendor
// requests) except for the four toggle-resetting requests intercepted above.
bool USBIPComponent::handle_cmd_submit_(uint32_t seqnum, uint32_t ep, uint32_t direction,
                                        uint32_t transfer_flags, uint32_t length,
                                        const uint8_t *setup, const uint8_t *out_data,
                                        bool stream_out) {
  // Anything that fails before the payload is consumed must still drain it.
  auto fail = [&](int32_t status) {
    if (stream_out) {
      uint8_t sink[512];
      uint32_t left = length;
      while (left && this->client_fd_ >= 0) {
        size_t n = left > sizeof(sink) ? sizeof(sink) : left;
        if (!this->read_bytes_(sink, n, 10000)) { this->disconnect_client_(); return true; }
        left -= n;
      }
    }
    this->send_urb_response_(seqnum, status, nullptr, 0);
    return true;
  };

  if (!this->usb_device_.connected || !this->usb_device_.enumerated || !usb_device_hdl)
    return fail(-LINUX_ENODEV);

  bool is_control = (ep == 0);
  uint8_t ep_addr = is_control ? 0 : (uint8_t)((ep & 0x0F) | (direction ? 0x80 : 0x00));

  if (direction && length > kMaxUrbInSize) {
    DEBUG_USB("IN URB seq=%lu too large (%lu bytes) — rejecting",
             (unsigned long)seqnum, (unsigned long)length);
    return fail(-LINUX_EIO);
  }

  uint16_t wLength = 0;
  bool ctrl_in = false;
  if (is_control) {
    uint8_t bmRequestType = setup[0], bRequest = setup[1];
    uint16_t wValue = setup[2] | (setup[3] << 8);
    wLength = setup[6] | (setup[7] << 8);
    ctrl_in = (bmRequestType & 0x80) != 0;
    if (bmRequestType == 0x00 && bRequest == USB_REQ_SET_CONFIGURATION) {
      this->handle_set_configuration_(seqnum, setup);
      return true;
    } else if (bmRequestType == 0x01 && bRequest == USB_REQ_SET_INTERFACE) {
      this->handle_set_interface_(seqnum, setup);
      return true;
    } else if (bmRequestType == 0x02 && bRequest == USB_REQ_CLEAR_FEATURE && wValue == 0x0000) {
      this->handle_clear_halt_(seqnum, setup);
      return true;
    } else if (bmRequestType == 0x23 && bRequest == USB_REQ_SET_FEATURE && wValue == 0x0004) {
      this->handle_reset_device_(seqnum, setup);  // USB_RT_PORT / PORT_FEAT_RESET
      return true;
    }
    if (!ctrl_in && length < wLength) {
      DEBUG_USB("Control OUT seq=%lu: wLength %u > buffer %lu", (unsigned long)seqnum,
               wLength, (unsigned long)length);
      return fail(-LINUX_EINVAL);
    }
    if (this->absorb_line_state_(seqnum, setup))
      return true;
  }
  // Anything else reaching the device must see the line state the client
  // believes is already applied (it was acked early). The early ack also
  // means the application's own wait between the line change and this
  // request — "assert DTR, sleep, send the wake-up byte", which is how K-line
  // and boot-mode cables are driven — has already elapsed on its side while
  // the adapter saw nothing; applying the lines now and sending the data
  // straight after would collapse that wait to zero. Re-insert it: hold this
  // URB for as long as the line state has been pending (at most the hold-off,
  // after which the poll loop has applied it anyway).
  if (s_line_state.pending) {
    int64_t waited_us = esp_timer_get_time() - s_line_state.first_us;
    this->flush_line_state_(true);
    if (waited_us > 0) {
      if (waited_us > (int64_t)USBIP_LINE_STATE_HOLD_MS * 1000)
        waited_us = (int64_t)USBIP_LINE_STATE_HOLD_MS * 1000;
      vTaskDelay(pdMS_TO_TICKS((waited_us + 999) / 1000));
    }
  }

  const EndpointInfo *ei = is_control ? nullptr : this->usb_device_.find_endpoint(ep_addr);
  uint8_t ep_type = is_control ? EP_TYPE_CONTROL : (ei ? (ei->attributes & 0x03) : EP_TYPE_BULK);

  PendingUrb *p = find_free_urb_slot();
  if (!p) {
    DEBUG_USB("Pending URB table full — rejecting seq=%lu", (unsigned long)seqnum);
    return fail(-LINUX_EIO);
  }
  this->last_urb_ms_ = millis();
  crumb::set(crumb::BRIDGE_SUBMIT);

  // Buffer sizing. ESP-IDF requires every IN transfer's num_bytes to be a whole
  // multiple of the endpoint MPS — control IN included: its own enumerator
  // rounds the data stage up to bMaxPacketSize0 (enum.c), and the DWC
  // descriptor DMA misbehaves on the 1–2 byte vendor reads serial adapters
  // and vendor devices live on when it isn't. URBs above kMaxTransferSize are
  // chunked: a big contiguous DMA alloc is unreliable on the no-PSRAM S3, so
  // IN payloads are parked chunk by chunk in heap and OUT payloads stream
  // from the socket, each moving through an mps-aligned 16K DMA transfer.
  uint16_t mps = is_control ? this->usb_device_.max_packet_size0 : this->mps_for_ep_(ep_addr);
  if (mps == 0) mps = is_control ? 8 : BULK_MPS;
  bool chunked = !is_control && (stream_out || length > kMaxTransferSize);
  size_t alloc_size, num_bytes = 0;
  if (is_control) {
    size_t data = ctrl_in ? ((size_t)wLength + mps - 1) / mps * mps : wLength;
    alloc_size = num_bytes = 8 + data;
  } else if (chunked) {
    alloc_size = (kMaxTransferSize / mps) * mps;
  } else if (direction) {
    num_bytes = ((length + mps - 1) / mps) * mps;
    if (num_bytes == 0) num_bytes = mps;
    alloc_size = num_bytes;
  } else {
    num_bytes = length;
    alloc_size = length ? length : 64;
  }

  usb_transfer_t *xfer = nullptr;
  if (usb_host_transfer_alloc(alloc_size, 0, &xfer) != ESP_OK || !xfer) {
    DEBUG_USB("URB alloc failed (%zu bytes)", alloc_size);
    return fail(-LINUX_EIO);
  }

  p->seqnum = seqnum;
  p->direction = direction;
  p->ep_addr = ep_addr;
  p->ep_type = ep_type;
  p->buflen = length;
  p->wlength = wLength;
  p->is_control = is_control;
  p->unlinked = false;
  p->killed = false;
  p->short_not_ok = (!is_control && direction && (transfer_flags & URB_SHORT_NOT_OK));
  p->done = false;
  p->status = USB_TRANSFER_STATUS_ERROR;
  p->actual = 0;
  p->submit_us = esp_timer_get_time();
  p->xfer = xfer;
  p->chunked_in = chunked && direction;
  p->in_nchunks = 0;
  p->stream_out = stream_out;
  p->accum_off = 0;
  p->chunk_bytes = 0;
  p->mps = mps;
  p->want_zlp = (!is_control && !direction && (transfer_flags & URB_ZERO_PACKET));
  p->active = true;

  xfer->device_handle = usb_device_hdl;
  xfer->callback = urb_transfer_cb;
  xfer->context = p;
  xfer->bEndpointAddress = ep_addr;
  xfer->timeout_ms = 0;  // no host-side timeout; CMD_UNLINK is the cancel path

  if (chunked) {
    if (stream_out) s_stream_urb = p;  // socket belongs to this URB until its payload is in
    esp_err_t err = this->submit_urb_chunk_(p);
    if (err != ESP_OK) {
      DEBUG_USB("Chunked URB first submit failed: seq=%lu %s",
               (unsigned long)seqnum, esp_err_to_name(err));
      bool net_fail = (err == ESP_ERR_TIMEOUT && stream_out);
      free_urb_slot(p);
      if (net_fail) this->disconnect_client_();
      else this->send_urb_response_(seqnum, -LINUX_EPIPE, nullptr, 0);
    }
    return true;
  }

  xfer->num_bytes = num_bytes;
  if (p->want_zlp)
    xfer->flags |= USB_TRANSFER_FLAG_ZERO_PACK;  // URB_ZERO_PACKET

  if (is_control) {
    memcpy(xfer->data_buffer, setup, 8);
    if (!ctrl_in && out_data && wLength > 0)
      memcpy(xfer->data_buffer + 8, out_data, wLength);
    ecu::on_control(0, setup, (!ctrl_in && wLength > 0) ? out_data : nullptr,
                    (!ctrl_in) ? wLength : 0);
  } else if (!direction && length > 0) {
    memcpy(xfer->data_buffer, out_data, length);
    if (is_tappable_ep_type(ep_type))
      ecu::on_bulk_out(ep & 0x0F, out_data, length);  // host→ECU command tap
  }

  esp_err_t err = is_control ? submit_control_retry(xfer)
                             : usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    // ESP_ERR_INVALID_STATE on a data pipe = still halted after a device
    // STALL; the client must CLEAR_FEATURE first. Report it as such.
    DEBUG_USB("URB submit failed: seq=%lu ep=0x%02X %s",
             (unsigned long)seqnum, ep_addr, esp_err_to_name(err));
    free_urb_slot(p);
    this->send_urb_response_(seqnum, err == ESP_ERR_INVALID_ARG ? -LINUX_EINVAL : -LINUX_EPIPE,
                             nullptr, 0);
  }
  return true;
}

// Poll the pending table for completions and answer them (reference engines'
// Phase-2 loop). Runs every loop() pass, including with no client attached, so
// orphaned/unlinked slots are always reaped.
void USBIPComponent::poll_pending_urbs_() {
  int64_t now = esp_timer_get_time();
  this->flush_line_state_(false);

  // Transient transfer errors (CRC/timeout/babble) halt the ESP-IDF pipe and
  // flush its queue, but a real host controller re-activates the queue by
  // itself — only a STALL waits for the driver's CLEAR_FEATURE. Mirror that,
  // or one bad packet on a marginal cable strands the endpoint until re-plug.
  // A transaction error advances no data toggle, so a plain clear is correct.
  if (usb_device_hdl && this->usb_device_.connected) {
    for (auto &p : s_pending) {
      if (!p.active || !p.done || p.is_control) continue;
      usb_transfer_status_t st = p.status;
      if (st == USB_TRANSFER_STATUS_ERROR || st == USB_TRANSFER_STATUS_TIMED_OUT ||
          st == USB_TRANSFER_STATUS_OVERFLOW)
        usb_host_endpoint_clear(usb_device_hdl, p.ep_addr);
    }
  }

  for (auto &p : s_pending) {
    if (!p.active) continue;

    if (!p.done) {
      // Live URBs never time out here — the client owns their lifetime. Only
      // orphans (unlinked but the stack hasn't completed them) get re-poked.
      if (p.unlinked && now - p.submit_us > kOrphanRetryUs) {
        cancel_pending_urb(&p, this->usb_device_.connected ? usb_device_hdl : nullptr);
        if (p.done) free_urb_slot(&p);
        else p.submit_us = now;  // still stuck: retry the abort next window
      }
      continue;
    }

    usb_transfer_status_t st = p.status;

    if (p.unlinked) {  // spec: no RET_SUBMIT after a successful UNLINK
      free_urb_slot(&p);
      continue;
    }

    if (st == USB_TRANSFER_STATUS_CANCELED) {
      if (p.killed) {
        // Endpoint was torn down under it (SET_CONFIGURATION / SET_INTERFACE).
        this->send_urb_response_(p.seqnum, -LINUX_ESHUTDOWN, nullptr, 0);
        free_urb_slot(&p);
        continue;
      }
      // Collateral cancel not yet resubmitted by the handler that caused it
      // (e.g. the stack flushed the pipe on its own): resubmit in order.
      if (usb_device_hdl && this->usb_device_.connected) {
        resubmit_canceled_urbs();
        if (!p.done) continue;  // back in flight
        st = p.status;          // resubmit failed: fall through with its status
        if (st == USB_TRANSFER_STATUS_CANCELED) st = USB_TRANSFER_STATUS_STALL;
      }
    }

    // Chunked URB: accumulate/advance this chunk, then continue or finish. A
    // short packet terminates the transfer early, same as a single URB would.
    if ((p.chunked_in || p.stream_out) && st == USB_TRANSFER_STATUS_COMPLETED) {
      uint32_t got = (uint32_t)p.actual;
      uint32_t room = p.buflen - p.accum_off;
      if (got > room) got = room;
      bool short_pkt = got < p.chunk_bytes;
      bool more = !short_pkt && p.accum_off + got < p.buflen;
      if (p.direction && got > 0) {
        if (is_tappable_ep_type(p.ep_type))
          ecu::on_bulk_in(p.ep_addr & 0x0F, p.xfer->data_buffer, got);
        // Park the finished transfer; if another chunk follows, it needs a
        // transfer of its own (same size, DMA-capable).
        usb_transfer_t *next = nullptr;
        bool ok = p.in_nchunks < kMaxInChunks;
        if (ok && more)
          ok = usb_host_transfer_alloc(p.xfer->data_buffer_size, 0, &next) == ESP_OK && next;
        if (!ok) {
          DEBUG_USB("IN chunk park failed: seq=%lu (heap blk=%u)", (unsigned long)p.seqnum,
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
          uint32_t seq = p.seqnum;
          free_urb_slot(&p);
          this->send_urb_response_(seq, -LINUX_EIO, nullptr, 0);
          continue;
        }
        p.in_chunk[p.in_nchunks] = p.xfer;
        p.in_chunk_len[p.in_nchunks] = got;
        p.in_nchunks++;
        if (more) {
          next->device_handle = usb_device_hdl;
          next->callback = urb_transfer_cb;
          next->context = &p;
          next->bEndpointAddress = p.ep_addr;
          next->timeout_ms = 0;
          p.xfer = next;
        } else {
          p.xfer = nullptr;  // everything is parked; nothing in flight
        }
      }
      p.accum_off += got;
      if (more) {
        esp_err_t err = this->submit_urb_chunk_(&p);
        if (err == ESP_OK) continue;  // next chunk in flight
        DEBUG_USB("Chunk submit failed: seq=%lu %s",
                 (unsigned long)p.seqnum, esp_err_to_name(err));
        bool net_fail = (err == ESP_ERR_TIMEOUT && p.stream_out);
        uint32_t seq = p.seqnum;
        p.done = true;
        free_urb_slot(&p);
        if (net_fail) this->disconnect_client_();
        else this->send_urb_response_(seq, -LINUX_EPIPE, nullptr, 0);
        continue;
      }
      int32_t rs = (p.short_not_ok && p.accum_off < p.buflen) ? -LINUX_EREMOTEIO : 0;
      // Header carries the total; the parked chunks follow in order.
      this->send_urb_response_(p.seqnum, rs, nullptr, p.accum_off);
      for (int i = 0; i < p.in_nchunks && this->client_fd_ >= 0; i++)
        this->write_bytes_(p.in_chunk[i]->data_buffer, p.in_chunk_len[i]);
      free_urb_slot(&p);
      continue;
    }

    // On STALL the ESP-IDF pipe stays halted — deliberately leave it that way.
    // The client drives recovery with CLEAR_FEATURE(HALT), which rebuilds the
    // pipes (handle_clear_halt_) before answering it.

    int32_t reply_status = map_usb_status(st);
    uint32_t len = 0;
    const uint8_t *data = nullptr;
    if (reply_status == 0) {
      int a = p.actual;
      if (p.is_control) {
        a = (a > 8) ? a - 8 : 0;
        if ((uint32_t)a > p.wlength) a = p.wlength;
      }
      if (a < 0) a = 0;
      if ((uint32_t)a > p.buflen) a = p.buflen;  // never exceed the request
      len = (uint32_t)a;
      if (p.direction && len > 0) {
        data = p.is_control ? p.xfer->data_buffer + 8 : p.xfer->data_buffer;
        if (p.is_control)
          ecu::on_control(1, p.xfer->data_buffer, data, len);  // setup is still at [0:8)
        if (is_tappable_ep_type(p.ep_type))
          ecu::on_bulk_in(p.ep_addr & 0x0F, data, len);  // ECU→host reply tap
      }
      if (p.short_not_ok && p.direction && len < p.buflen)
        reply_status = -LINUX_EREMOTEIO;  // URB_SHORT_NOT_OK: short read is an error
    }
    TRACE_URB("URB seq=%lu latency: xfer=%ldus total=%ldus",
              (unsigned long)p.seqnum, (long)(p.done_us - p.submit_us),
              (long)(now - p.submit_us));
    this->send_urb_response_(p.seqnum, reply_status, data, len);
    free_urb_slot(&p);
  }
}

// Max packet size of an endpoint from its parsed descriptor. ep0 uses the
// device descriptor's bMaxPacketSize0; unknown endpoints fall back to the
// Full-Speed bulk default.
uint16_t USBIPComponent::mps_for_ep_(uint8_t ep_addr) const {
  if ((ep_addr & 0x0F) == 0) return this->usb_device_.max_packet_size0;
  const EndpointInfo *ei = this->usb_device_.find_endpoint(ep_addr);
  if (ei && ei->max_packet_size) return ei->max_packet_size;
  return BULK_MPS;
}

bool USBIPComponent::active_within(uint32_t window_ms) const {
  uint32_t t = this->last_urb_ms_;
  return t != 0 && (uint32_t)(millis() - t) < window_ms;
}

void USBIPComponent::send_urb_response_(uint32_t seqnum, int32_t status,
                                         const uint8_t *data, uint32_t length) {
  this->last_urb_ms_ = millis();
  if (status != 0) {
    TRACE_URB("URB response: seq=%lu status=%ld len=%lu",
              (unsigned long)seqnum, (long)status, (unsigned long)length);
  } else {
    TRACE_URB("URB response: seq=%lu status=OK len=%lu",
              (unsigned long)seqnum, (unsigned long)length);
  }

  USBIPRetSubmit response;
  memset(&response, 0, sizeof(response));

  response.command = htonl_(USBIP_RET_SUBMIT);
  response.seqnum = htonl_(seqnum);
  response.devid = 0;
  response.direction = 0;
  response.ep = 0;
  response.status = htonl_(static_cast<uint32_t>(status));
  response.actual_length = htonl_(length);
  response.start_frame = 0;
  // Per USB/IP spec: number_of_packets is 0xFFFFFFFF for non-ISO transfers.
  response.number_of_packets = htonl_(0xFFFFFFFF);
  response.error_count = 0;

  // Header and a small payload go out in ONE send(): with TCP_NODELAY every
  // send() is its own segment, and a USB-serial adapter answering its bulk-IN
  // read every latency-timer tick with a 2-byte status packet (FTDI at the
  // 1–2 ms VCDS uses: ~1k replies/s) was costing two segments per reply —
  // packet rate, not bytes, is what limits the ESP32's WiFi/lwIP path.
  static constexpr size_t kCoalesceMax = 512;
  if (data && length > 0 && length <= kCoalesceMax) {
    uint8_t frame[sizeof(response) + kCoalesceMax];
    memcpy(frame, &response, sizeof(response));
    memcpy(frame + sizeof(response), data, length);
    this->write_bytes_(frame, sizeof(response) + length);
    return;
  }

  this->write_bytes_(reinterpret_cast<uint8_t *>(&response), sizeof(response));

  if (data && length > 0) {
    this->write_bytes_(data, length);
  }
}

void USBIPComponent::disconnect_client_() {
  if (this->client_fd_ >= 0) {
    close(this->client_fd_);
    this->client_fd_ = -1;
  }
  this->client_state_ = CLIENT_STATE_IDLE;
  s_stream_urb = nullptr;
  this->flush_line_state_(true);  // last requested DTR/RTS state, then done
  // Abort every pending URB — there is no client left to answer. Slots whose
  // callback hasn't fired yet stay active (unlinked) and are reaped by
  // poll_pending_urbs_() once the callback lands; freeing earlier would be a
  // use-after-free of an in-flight transfer.
  for (auto &p : s_pending) {
    if (!p.active) continue;
    p.unlinked = true;
    cancel_pending_urb(&p, this->usb_device_.connected ? usb_device_hdl : nullptr);
    if (p.done) free_urb_slot(&p);
  }
  DEBUG_USB("Client disconnected");
}

bool USBIPComponent::read_bytes_(uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  if (this->client_fd_ < 0) {
    return false;
  }

  size_t total_read = 0;
  uint32_t start = millis();

  while (total_read < length && (millis() - start) < timeout_ms) {
    ssize_t n = recv(this->client_fd_, buffer + total_read, length - total_read, 0);
    if (n > 0) {
      total_read += n;
    } else if (n == 0) {
      // Connection closed
      return false;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    } else {
      // Feed watchdog and yield while waiting for data
      esp_task_wdt_reset();
      vTaskDelay(1);
    }
  }

  return total_read == length;
}

bool USBIPComponent::write_bytes_(const uint8_t *buffer, size_t length) {
  if (this->client_fd_ < 0) {
    return false;
  }

  size_t total_written = 0;
  uint32_t start = millis();
  while (total_written < length) {
    ssize_t n = send(this->client_fd_, buffer + total_written, length - total_written, 0);
    if (n > 0) {
      total_written += n;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
      // ENOMEM is lwIP failing to get a pbuf under heap pressure — transient
      // while a big reply drains; retry it like a full window, don't drop
      // the client (which reads as a failed drive on the laptop).
      return false;
    } else {
      // Stalled client (full socket buffer): give up after 10 s instead of
      // blocking the bridge loop forever — the client will be disconnected.
      // Generous on purpose: a laptop's WiFi power-save hiccup can stall the
      // window for seconds, and a dropped session reads as a failed drive.
      if (millis() - start > 10000) {
        DEBUG_USB("write_bytes_ timeout — dropping client");
        this->disconnect_client_();
        return false;
      }
      esp_task_wdt_reset();
      vTaskDelay(1);
    }
  }

  return total_written == length;
}

uint16_t USBIPComponent::htons_(uint16_t value) {
  return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

uint32_t USBIPComponent::htonl_(uint32_t value) {
  return ((value & 0xFF) << 24) |
         ((value & 0xFF00) << 8) |
         ((value >> 8) & 0xFF00) |
         ((value >> 24) & 0xFF);
}

uint16_t USBIPComponent::ntohs_(uint16_t value) {
  return htons_(value);
}

uint32_t USBIPComponent::ntohl_(uint32_t value) {
  return htonl_(value);
}

}  // namespace usbip
