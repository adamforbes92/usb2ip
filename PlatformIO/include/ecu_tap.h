#pragma once

// IgnitronUSB — raw USB-frame tap.
//
// Called from the USB/IP bridge on every ECU bulk transfer. Does two jobs
// without ever blocking the forward path:
//   1. feeds bulk-IN payloads to the gauge Decoder, and
//   2. queues a copy of each transfer for the capture server (TCP tap port),
//      so a laptop can record the raw frames while Ignitron shows gauges.
//
// Wire record format streamed on the tap port (all integers little-endian):
//   u8  magic = 0xEC
//   u8  dir   (0 = OUT host->ECU, 1 = IN ECU->host, 0xFF = text note marker)
//   u8  ep    (endpoint number, 0..15)
//   u8  flags (reserved, 0)
//   u32 t_ms  (millis timestamp)
//   u16 len   (payload length)
//   u8  payload[len]

#include <cstdint>
#include <cstddef>

namespace ecu {

// Bridge hooks (call from the USB host task).
void on_bulk_in(uint8_t ep, const uint8_t *data, size_t len);
void on_bulk_out(uint8_t ep, const uint8_t *data, size_t len);

// Control-transfer tap: setup8 is the raw 8-byte USB setup packet, data/len is
// whichever data stage actually happened (nullptr/0 if none). dir 0 = the
// request as submitted (setup + any host->device data), 1 = the device's
// response data once it completes. Always recorded at ep=0, so captures can
// tell it apart from bulk/interrupt traffic on ep1+. This is the only way to
// see the session handshake (VID/PID reads aside, most of it lives in vendor
// control requests, not the bulk endpoint the gauge tap already covers).
void on_control(uint8_t dir, const uint8_t *setup8, const uint8_t *data, size_t len);

// Start the capture server + decoder feed. port 0 disables the TCP server
// (the decoder still runs). Safe to call once from setup().
void tap_begin(uint16_t tcp_port);

// Per-frame serial hex dump (off by default — high-rate gauge data floods UART).
void tap_set_serial_dump(bool on);

// --- in-browser capture (Diagnostics → "USB Capture") ---------------------
// A second consumer of the same record ring, so an end user can record a raw
// USB session from the dashboard and hand us the .bin — same format tapcap.py
// writes, so every existing tool reads it. The ring has one owner at a time:
// open() fails while a TCP tapcap client is attached (and vice versa).
bool   tap_capture_open();                          // claim the ring, reset it
void   tap_capture_close();                         // release the ring
bool   tap_capture_active();                        // a browser capture is running
size_t tap_capture_read(uint8_t *out, size_t cap);  // drain queued records
// Push a marker record (dir = 0xFF, ep = 0, payload = UTF-8 text) so the user
// can annotate the stream ("idle", "WOT 3rd gear") the way tapcap --note did.
void   tap_note(const char *text);

// Rolling counters for the 1 Hz telemetry line.
struct TapStats {
  uint32_t in_frames;
  uint32_t in_bytes;
  uint32_t out_frames;
  uint32_t out_bytes;
  uint32_t dropped;      // ring-buffer overflows
  bool client_connected; // a capture client is attached
};
TapStats tap_stats();

}  // namespace ecu
