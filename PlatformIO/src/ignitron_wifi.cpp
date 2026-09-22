#include "ignitron_wifi.h"
#include "defs.h"
#include "telemetry.h"
#include "port_control.h"
#include "ecu_tap.h"
#include "gauges.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "usbip_bridge.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <cstring>
#include "crumb.h"
#include "logger.h"
#include <memory>

AsyncWebServer server(WEB_HTTP_PORT);

static bool s_wifi_on = false;
static Preferences s_wifi_prefs;
static volatile uint32_t s_reboot_at = 0;   // millis() deadline, 0 = no reboot pending

// Credentials are cached in RAM so the hot /api/status path never touches NVS.
// isKey() guards keep NVS from logging "NOT_FOUND" errors on a fresh device.
static bool s_creds_loaded = false;
static String s_cached_ssid;
static String s_cached_pass;
static uint8_t s_cached_chan = AP_CHANNEL;

// Band scan state (Settings → WiFi channel). The scan needs the STA interface,
// so the radio hops AP+STA for its duration (~2 s) and back; associated
// stations see a brief hiccup, which is why it is on-demand only.
static bool s_scan_running = false;

// --- in-browser USB capture (Diagnostics → USB Capture) ----------------------
// The dashboard opens ws://<board>/ws/capture; we stream the tap ring to it as
// binary frames (tapcap record format, see ecu_tap.h) and the browser stitches
// them into one .bin the user can download and send in. Text frames coming
// back are user notes and get stamped into the stream as 0xFF marker records.
// One client at a time — the ring has a single reader.
static AsyncWebSocket s_capWs("/ws/capture");
static uint32_t s_capClientId = 0;   // 0 = nobody capturing via the browser
static uint32_t s_capBytes = 0;
static uint32_t s_capStartMs = 0;

static void capWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type,
                       void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    if (s_capClientId != 0 || !ecu::tap_capture_open())
    {
      // Either a second tab or the TCP tapcap client already has the ring.
      client->text("{\"ok\":false,\"err\":\"a capture is already running\"}");
      client->close();
      return;
    }
    s_capClientId = client->id();
    s_capBytes = 0;
    s_capStartMs = millis();
    {
      // First record: a self-describing header so a .bin sent in cold still
      // tells us what firmware/device produced it.
      char meta[160];
      uint16_t vid = 0, pid = 0;
      bool poll = usbip::gauge_poll_active();
      bool ipc = usbip::g_usbip_component && usbip::g_usbip_component->client_attached();
      if (usbip::g_usbip_component) {
        usbip::USBIPComponent::DeviceInfo dev = usbip::g_usbip_component->device_info();
        vid = dev.vid; pid = dev.pid;
      }
      snprintf(meta, sizeof(meta), "meta fw=%s dev=%04X:%04X gaugePoll=%d usbipClient=%d",
               FW_VERSION, vid, pid, poll, ipc);
      ecu::tap_note(meta);
    }
    client->text("{\"ok\":true}");
    DEBUG_WEB("capture ws client #%lu connected", (unsigned long)client->id());
    break;

  case WS_EVT_DISCONNECT:
  case WS_EVT_ERROR:
    if (client->id() == s_capClientId)
    {
      s_capClientId = 0;
      ecu::tap_capture_close();
      DEBUG_WEB("capture ws closed after %lu bytes", (unsigned long)s_capBytes);
    }
    break;

  case WS_EVT_DATA:
  {
    if (client->id() != s_capClientId) break;
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    // Notes are short single-frame text messages; anything else is ignored.
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
      char note[201];
      size_t n = len < 200 ? len : 200;
      memcpy(note, data, n);
      note[n] = 0;
      ecu::tap_note(note);
    }
    break;
  }
  default:
    break;
  }
}

// Called from wifiLoop() (20 ms). Moves whatever the tap has queued into the
// WebSocket send queue, a few KB per tick, backing off while the queue is full
// so a slow phone link doesn't make AsyncTCP eat the heap.
static void capPump()
{
  if (s_capClientId == 0) return;
  AsyncWebSocketClient *c = s_capWs.client(s_capClientId);
  if (!c || c->status() != WS_CONNECTED)
  {
    s_capClientId = 0;
    ecu::tap_capture_close();
    return;
  }
  static uint8_t buf[2048];
  for (int i = 0; i < 4 && !c->queueIsFull(); i++)
  {
    size_t n = ecu::tap_capture_read(buf, sizeof(buf));
    if (n == 0) break;
    c->binary(buf, n);
    s_capBytes += n;
  }
  s_capWs.cleanupClients(2);
}

static void loadCredsOnce()
{
  if (s_creds_loaded)
    return;
  s_wifi_prefs.begin("wifi", true);
  s_cached_ssid = s_wifi_prefs.isKey("ssid") ? s_wifi_prefs.getString("ssid", AP_SSID) : String(AP_SSID);
  s_cached_pass = s_wifi_prefs.isKey("pass") ? s_wifi_prefs.getString("pass", AP_PASSWORD) : String(AP_PASSWORD);
  s_cached_chan = s_wifi_prefs.isKey("chan") ? s_wifi_prefs.getUChar("chan", AP_CHANNEL) : AP_CHANNEL;
  if (s_cached_chan < 1 || s_cached_chan > AP_COUNTRY_NCHAN) s_cached_chan = AP_CHANNEL;
  s_wifi_prefs.end();
  s_creds_loaded = true;
}

uint8_t wifiApChannel()
{
  loadCredsOnce();
  return s_cached_chan;
}

void wifiSetChannel(uint8_t channel)
{
  if (channel < 1 || channel > AP_COUNTRY_NCHAN) return;
  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.putUChar("chan", channel);
  s_wifi_prefs.end();
  s_cached_chan = channel;
  DEBUG_WIFI("channel saved: %u (applies on reboot)", channel);
}

String wifiApSsid()
{
  loadCredsOnce();
  return s_cached_ssid;
}

bool wifiApHasPassword()
{
  loadCredsOnce();
  return s_cached_pass.length() >= 8;
}

void wifiSetCredentials(const String &ssid, const String &password)
{
  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.putString("ssid", ssid);
  s_wifi_prefs.putString("pass", password);
  s_wifi_prefs.end();
  s_cached_ssid = ssid;
  s_cached_pass = password;
  s_creds_loaded = true;
  DEBUG_WIFI("credentials saved: ssid=\"%s\" secured=%d", ssid.c_str(), password.length() >= 8);
}

void wifiFactoryReset()
{
  DEBUG_WIFI("factory reset — clearing WiFi creds + settings");
  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.clear();
  s_wifi_prefs.end();
  s_cached_ssid = AP_SSID;
  s_cached_pass = AP_PASSWORD;
  s_cached_chan = AP_CHANNEL;
  s_creds_loaded = true;
  Preferences p;
  p.begin("ignitron", false);   // port-control settings namespace
  p.clear();
  p.end();
}

void requestReboot() { s_reboot_at = millis() + 400; }

void wifiLoop()
{
  capPump();
  wifiManagerTick(); // Home WiFi (bridge mode): connection tracking + retry back-off
  if (s_reboot_at && (int32_t)(millis() - s_reboot_at) >= 0)
  {
    DEBUG_WIFI("rebooting to apply changes");
    Serial.flush();
    ESP.restart();
  }
}

bool wifiBridgeStart()
{
  if (s_wifi_on)
    return true;

  loadCredsOnce();
  String ssid = s_cached_ssid;
  String pass = s_cached_pass;

  WiFi.mode(WIFI_AP);
  // Regulatory domain first: the driver's default (cc=01) only permits
  // channels 1-11, and softAP() on 12/13 then fails outright.
  wifi_country_t country = {};
  memcpy(country.cc, AP_COUNTRY, 2);
  country.schan = 1;
  country.nchan = AP_COUNTRY_NCHAN;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);

  IPAddress ip(AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  // WPA2 needs >= 8 chars; anything shorter (incl. empty) => open network.
  uint8_t chan = wifiApChannel();
  auto start_ap = [&](uint8_t ch) {
    return (pass.length() >= 8) ? WiFi.softAP(ssid.c_str(), pass.c_str(), ch)
                                : WiFi.softAP(ssid.c_str(), nullptr, ch);
  };
  bool ok = start_ap(chan);
  if (!ok && chan != AP_CHANNEL)
  {
    // A saved channel the driver rejects must never take the bridge down.
    DEBUG_WIFI("soft-AP refused channel %u — falling back to channel %u", chan, AP_CHANNEL);
    chan = AP_CHANNEL;
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    ok = start_ap(chan);
  }
  s_wifi_on = ok;
  if (ok)
  {
    // Throughput tuning for the USB/IP bridge. USB/IP is a tight request/reply
    // protocol, so link latency dominates over raw bandwidth.
    //  - Power-save (default WIFI_PS_MIN_MODEM) parks the radio between beacons
    //    and adds ~100 ms latency per exchange — the single biggest throughput
    //    killer here. Force it off.
    //  - Enable the full b/g/n protocol set so the link negotiates 802.11n MCS
    //    instead of falling back to 11g. Stay on HT20: the bridge needs only a
    //    few Mbit/s, and a 40 MHz channel in the 2.4 GHz band collides with
    //    twice the neighbours — each lost frame is a lwIP retransmit-timer
    //    stall on the USB/IP stream, which hurts far more than PHY rate helps.
    //  - Push TX power to the maximum for a cleaner link / higher sustained MCS.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(WIFI_IF_AP,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(80);  // 0.25 dBm units → 20 dBm
    DEBUG_WIFI("soft-AP \"%s\" up (%s), IP %s [ch%d ps=off 11bgn HT20]", ssid.c_str(),
               pass.length() >= 8 ? "WPA2" : "OPEN", WiFi.softAPIP().toString().c_str(),
               chan);
    // Home WiFi (bridge mode, shared wifi_manager): join the saved home router
    // as a station alongside this AP so a phone on that network reaches the
    // board and the internet at once (needed for "Update from GitHub").
    wifiManagerStaStart();
  }
  else
    DEBUG_WIFI("soft-AP start FAILED");
  return ok;
}

void wifiBridgeStop()
{
  if (!s_wifi_on)
    return;
  wifiManagerStaStop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_wifi_on = false;
  DEBUG_WIFI("soft-AP down (bridge off)");
}

bool wifiBridgeActive() { return s_wifi_on; }

// ota_manager hook: a filesystem update unmounts LittleFS before rewriting the
// partition, so a running data-log (which writes to /logs) must be closed first.
void otaOnStart(bool filesystem)
{
  if (filesystem && logger::running())
  {
    DEBUG_WEB("filesystem update starting - stopping the data logger");
    logger::stop();
  }
}

static const char *muxName(port::MuxMode m)
{
  switch (m)
  {
  case port::MuxMode::ESP_HOST: return "esp";
  case port::MuxMode::LAPTOP:   return "laptop";
  default:                      return "off";
  }
}

static const char *routeName(port::RouteMode m)
{
  switch (m)
  {
  case port::RouteMode::USBC: return "usbc";
  case port::RouteMode::WIFI: return "wifi";
  default:                    return "auto";
  }
}

void setupWebRoutes()
{
  // Shared OTA + Home WiFi routes FIRST: ota_manager's first route carries the
  // filter that notes web activity for every request (otaWebClientActive()),
  // and /api/wifi/sta must precede our own /api/wifi routes (the server
  // matches "<uri>/..." prefixes too). Two-stage OTA by design: the filesystem
  // image carries the web UI and does NOT reboot, the firmware upload follows
  // it and reboots once at the end. See ota_manager.h.
  otaManagerAttach(server);
  wifiManagerAttachSta(server);

  // Live telemetry — polled by the dashboard once per interval.
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    crumb::set(crumb::WEB_STATUS);
    ecu::TelemetryState s = ecu::g_telemetry.snapshot();

    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    doc["link"] = s.link_ok;
    doc["frames"] = s.frame_count;
    doc["ageMs"] = millis() - s.last_update_ms;
    doc["heap"] = ESP.getFreeHeap();

    JsonObject ch = doc["ch"].to<JsonObject>();
    for (uint8_t i = 0; i < ecu::CH_COUNT; i++) {
      if (!s.valid[i]) continue;
      ch[ecu::channel_meta((ecu::Channel)i).key] = s.values[i];
    }

    // Fault / limp status — always published, independent of gauge selection.
    //   ch 15  ECU number of faults in DSP     ch 271 ECU number of faults in CPU
    //   ch 218 ECU last fault code (numeric P-code, names in data/faults.js)
    //   ch 211 bit 11 "ECU limp mode active"  ch 196 bit 9 "Switches: clear limp mode"
    {
      uint16_t dsp = 0, cpu = 0, last = 0, w211 = 0, w196 = 0;
      bool ok = gauges::raw_channel(271, cpu) & gauges::raw_channel(218, last) &
                gauges::raw_channel(211, w211) & gauges::raw_channel(196, w196);
      bool ok_dsp = gauges::raw_channel(15, dsp);
      JsonObject f = doc["fault"].to<JsonObject>();
      f["valid"] = ok;
      if (ok) {
        f["cpu"] = cpu;
        f["dsp"] = ok_dsp ? dsp : 0;
        f["last"] = last;
        f["limp"] = (w211 >> 11) & 1;
        f["clearSw"] = (w196 >> 9) & 1;
      }
    }

    // Registry gauges: only the ones the user switched on, so the dashboard
    // payload stays small no matter how big the table grows.
    JsonObject g = doc["g"].to<JsonObject>();
    for (size_t i = 0; i < gauges::count(); i++) {
      if (!gauges::enabled(i) || !gauges::valid(i)) continue;
      g[gauges::def(i).key] = gauges::value(i);
    }

    // ECU identity: bootloader/firmware version of each processor, decoded
    // from the session-start identity reads (PC-driven or our own replay).
    JsonObject id = doc["ident"].to<JsonObject>();
    auto put = [&](const char *key, const ecu::EcuUnitVersion &u) {
      if (!u.valid) return;
      JsonObject o = id[key].to<JsonObject>();
      char buf[16];
      snprintf(buf, sizeof(buf), "%u.%u", u.bl_major, u.bl_minor); o["bl"] = buf;
      snprintf(buf, sizeof(buf), "%u.%u", u.fw_major, u.fw_minor); o["fw"] = buf;
    };
    put("com", s.ident.com);
    put("dsp", s.ident.dsp);
    put("cpu", s.ident.cpu);

    // Board state: USB routing + ECU power + WiFi/idle status.
    port::Status ps = port::g_port.status();
    port::Settings cfg = port::g_port.settings();
    JsonObject sys = doc["sys"].to<JsonObject>();
    sys["usbc"] = ps.usbc_present;
    sys["ecuPwr"] = ps.ecu_powered;
    sys["wifiOn"] = ps.wifi_on;
    sys["wifiUse"] = ps.wifi_in_use;
    sys["asleep"] = ps.asleep;
    sys["mux"] = muxName(ps.mux);
    sys["route"] = routeName(ps.route);
    sys["idleMs"] = ps.idle_remaining_ms;
    sys["cDisWifi"] = cfg.usbc_disables_wifi;
    sys["cResWifi"] = cfg.restore_wifi_on_unplug;
    sys["idleOff"] = cfg.idle_auto_off;
    sys["ssid"] = wifiApSsid();
    sys["ip"] = WiFi.softAPIP().toString();
    sys["gaugePoll"] = usbip::gauge_poll_active();

    // USB connectivity + data-flow trace (Diag tab).
    ecu::TapStats ts = ecu::tap_stats();
    JsonObject usb = doc["usb"].to<JsonObject>();
    usb["ipClient"] = usbip::g_usbip_component && usbip::g_usbip_component->client_attached();
    usb["capClient"] = ts.client_connected;
    usb["capBrowser"] = s_capClientId != 0;   // browser capture running (vs TCP tapcap)
    usb["capBytes"] = s_capBytes;
    usb["inFrames"] = ts.in_frames;
    usb["outFrames"] = ts.out_frames;
    usb["inBytes"] = ts.in_bytes;
    usb["outBytes"] = ts.out_bytes;
    usb["dropped"] = ts.dropped;

    // Attached USB device (what's plugged into the USB-A port).
    if (usbip::g_usbip_component) {
      usbip::USBIPComponent::DeviceInfo dev = usbip::g_usbip_component->device_info();
      JsonObject d = usb["dev"].to<JsonObject>();
      d["present"] = dev.present;
      d["ready"] = dev.ready;
      d["vid"] = dev.vid;
      d["pid"] = dev.pid;
      d["class"] = dev.usb_class;
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response); });

  // Persisted settings (stage 2/3 optional behaviour). POST with query params:
  //   /api/settings?usbcDisablesWifi=1&restoreWifiOnUnplug=0
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    port::Settings s = port::g_port.settings();
    if (request->hasParam("usbcDisablesWifi"))
      s.usbc_disables_wifi = request->getParam("usbcDisablesWifi")->value().toInt() != 0;
    if (request->hasParam("restoreWifiOnUnplug"))
      s.restore_wifi_on_unplug = request->getParam("restoreWifiOnUnplug")->value().toInt() != 0;
    if (request->hasParam("idleAutoOff"))
      s.idle_auto_off = request->getParam("idleAutoOff")->value().toInt() != 0;
    port::g_port.set_settings(s);
    request->send(200, "application/json", "{\"ok\":true}"); });

  // Re-power the ECU and restart the 5-minute idle window.
  server.on("/api/wake", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    port::g_port.wake();
    request->send(200, "application/json", "{\"ok\":true}"); });

  // Replay the captured session-start handshake so the ECU begins streaming
  // telemetry without a real USB/IP client attached ("Launch Gauges").
  server.on("/api/launch", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    // wake() unconditionally toggles the USB mux (break-before-make, ~2ms
    // Hi-Z) even when already ESP-hosted, which disconnects/re-enumerates
    // the ECU right before the replay tries to use it. Only wake if the ECU
    // is actually asleep/off/elsewhere — replay_ecu_handshake() below still
    // waits out a re-enumeration when one really is needed.
    port::Status ps = port::g_port.status();
    if (ps.asleep || !ps.ecu_powered || ps.mux != port::MuxMode::ESP_HOST)
      port::g_port.wake();
    bool ok = usbip::replay_ecu_handshake();
    if (ok) usbip::set_gauge_poll(true);
    JsonDocument doc;
    doc["ok"] = ok;
    if (!ok) doc["err"] = usbip::last_launch_error();
    String body;
    serializeJson(doc, body);
    request->send(ok ? 200 : 503, "application/json", body); });

  // Gauge registry: the full table for the Gauges tab. Values themselves ride
  // on /api/status; this is the (rarely changing) catalogue + on/off state.
  // The catalogue is ~384 rows, so serialising it whole would mean a ~40 KB
  // String on the heap next to the WiFi buffers. Fetch it a group at a time:
  //   GET /api/gauges            -> metadata only (group/type names, counts)
  //   GET /api/gauges?group=N    -> the rows in that group
  server.on("/api/gauges", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["arrangeByType"] = gauges::arrange_by_type();
    doc["sweep"] = gauges::sweep_on_launch();
    doc["autoLaunch"] = gauges::auto_launch();
    doc["hero"] = gauges::hero_key();
    {
      JsonArray order = doc["order"].to<JsonArray>();
      const char *p = gauges::order();
      while (p && *p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) order.add(String(p).substring(0, n));
        p = e ? e + 1 : nullptr;
      }
    }

    int want = request->hasParam("group") ? request->getParam("group")->value().toInt() : -1;
    if (want < 0) {
      JsonArray groups = doc["groups"].to<JsonArray>();
      for (uint8_t i = 0; i < gauges::GRP_COUNT; i++) groups.add(gauges::group_name(i));
      JsonArray types = doc["types"].to<JsonArray>();
      for (uint8_t i = 0; i < gauges::TYPE_COUNT; i++) types.add(gauges::type_name(i));
      size_t mapped = 0;
      for (size_t i = 0; i < gauges::count(); i++) if (gauges::mapped(i)) mapped++;
      doc["total"] = gauges::count();
      doc["mapped"] = mapped;
      doc["enabled"] = gauges::enabled_count();
      doc["maxEnabled"] = gauges::MAX_ENABLED;
    } else {
      doc["group"] = want;
      JsonArray arr = doc["gauges"].to<JsonArray>();
      for (size_t i = 0; i < gauges::count(); i++) {
        const gauges::GaugeDef &d = gauges::def(i);
        if (d.group != want) continue;
        JsonObject o = arr.add<JsonObject>();
        o["key"] = d.key;
        o["label"] = d.label;
        o["unit"] = d.unit;
        o["group"] = d.group;
        o["type"] = d.type;
        o["min"] = d.dmin;
        o["max"] = d.dmax;
        o["dp"] = d.dp;
        if (d.ch != gauges::CH_NOT_SENT) o["ch"] = d.ch;   // ECU channel index (0..383)
        o["on"] = gauges::enabled(i);
        o["mapped"] = gauges::mapped(i);
      }
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out); });

  // Toggle one gauge:            POST /api/gauges?key=tps&on=1
  // Dashboard preferences:        POST /api/gauges?arrangeByType=0|1&sweep=0|1
  //                                    &autoLaunch=0|1&hero=<key>&order=k1,k2,...
  server.on("/api/gauges", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("arrangeByType"))
      gauges::set_arrange_by_type(request->getParam("arrangeByType")->value().toInt() != 0);
    if (request->hasParam("sweep"))
      gauges::set_sweep_on_launch(request->getParam("sweep")->value().toInt() != 0);
    if (request->hasParam("autoLaunch"))
      gauges::set_auto_launch(request->getParam("autoLaunch")->value().toInt() != 0);
    if (request->hasParam("hero")) {
      if (!gauges::set_hero_key(request->getParam("hero")->value().c_str())) {
        request->send(404, "application/json", "{\"ok\":false,\"err\":\"unknown key\"}");
        return;
      }
    }
    if (request->hasParam("order"))
      gauges::set_order(request->getParam("order")->value().c_str());
    if (request->hasParam("key")) {
      int idx = gauges::index_of(request->getParam("key")->value().c_str());
      if (idx < 0) {
        request->send(404, "application/json", "{\"ok\":false,\"err\":\"unknown key\"}");
        return;
      }
      bool on = request->hasParam("on") && request->getParam("on")->value().toInt() != 0;
      if (!gauges::set_enabled((size_t)idx, on)) {
        const char *why = gauges::mapped((size_t)idx) ? "limit reached" : "not sent by the ECU";
        String body = String("{\"ok\":false,\"err\":\"") + why +
                      "\",\"enabled\":" + gauges::enabled_count() +
                      ",\"maxEnabled\":" + (unsigned)gauges::MAX_ENABLED + "}";
        request->send(409, "application/json", body);
        return;
      }
    }
    String body = String("{\"ok\":true,\"enabled\":") + gauges::enabled_count() +
                  ",\"maxEnabled\":" + (unsigned)gauges::MAX_ENABLED + "}";
    request->send(200, "application/json", body); });

  // Stop the standalone gauge poll ("Unlaunch Gauges"). Does not touch ECU
  // power or the WiFi bridge — see the app.js comment on why.
  server.on("/api/unlaunch", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    usbip::set_gauge_poll(false);
    request->send(200, "application/json", "{\"ok\":true}"); });

  // ECU fault memory: the same read Ignitron's "Fault codes" window does.
  //   GET  /api/faults        -> {ok, dsp:{counter,ok,csum}, cpu:{...}, faults:[{code,src,rpm,load,value,count,duration,pageOk}]}
  //   POST /api/faults/clear  -> {ok[,err]}  (wipes both tables and commits, as Ignitron does)
  server.on("/api/faults", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    static usbip::EcuFaultMemory mem;   // 1.3 KB: keep it off the web task's stack
    bool ok = usbip::ecu_read_faults(mem);
    JsonDocument doc;
    doc["ok"] = ok;
    if (!ok) doc["err"] = usbip::last_launch_error();
    JsonObject d = doc["dsp"].to<JsonObject>();
    d["ok"] = mem.dsp_ok; d["counter"] = mem.dsp_counter; d["csum"] = mem.dsp_csum;
    JsonObject c = doc["cpu"].to<JsonObject>();
    c["ok"] = mem.cpu_ok; c["counter"] = mem.cpu_counter; c["csum"] = mem.cpu_csum;
    JsonArray arr = doc["faults"].to<JsonArray>();
    for (size_t i = 0; i < mem.count; i++) {
      const usbip::EcuFault &f = mem.faults[i];
      JsonObject o = arr.add<JsonObject>();
      o["code"] = f.code; o["src"] = f.src == 1 ? "DSP" : "CPU";
      o["rpm"] = f.rpm; o["load"] = f.load; o["value"] = f.value;
      o["count"] = f.count; o["duration"] = f.duration; o["pageOk"] = f.page_ok;
    }
    String out;
    serializeJson(doc, out);
    request->send(ok ? 200 : 503, "application/json", out); });

  server.on("/api/faults/clear", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    bool ok = usbip::ecu_clear_faults();
    JsonDocument doc;
    doc["ok"] = ok;
    if (!ok) doc["err"] = usbip::last_launch_error();
    String out;
    serializeJson(doc, out);
    request->send(ok ? 200 : 503, "application/json", out); });

  // Manual USB routing override. POST /api/route?mode=auto|usbc|wifi
  // VBUS auto-detect is unreliable on this PCB, so the UI pins the ECU route.
  server.on("/api/route", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    String mode = request->hasParam("mode") ? request->getParam("mode")->value() : "";
    port::RouteMode m;
    if (mode == "usbc")      m = port::RouteMode::USBC;
    else if (mode == "wifi") m = port::RouteMode::WIFI;
    else if (mode == "auto") m = port::RouteMode::AUTO;
    else {
      request->send(400, "application/json", "{\"ok\":false,\"err\":\"mode must be auto|usbc|wifi\"}");
      return;
    }
    port::g_port.set_route_override(m);
    request->send(200, "application/json", "{\"ok\":true}"); });

  // WiFi soft-AP credentials. GET returns the current SSID + whether it is
  // secured (the password itself is never echoed back).
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["ssid"] = wifiApSsid();
    doc["secured"] = wifiApHasPassword();
    doc["channel"] = wifiApChannel();
    doc["maxChannel"] = AP_COUNTRY_NCHAN;
    IPAddress ip(AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3);
    doc["ip"] = ip.toString();
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response); });

  // Update SSID/password. POST /api/wifi?ssid=..&password=..
  // Empty password => open network. A non-empty password must be >= 8 chars.
  // Saves to NVS and reboots to apply (the AP restarts on the new credentials).
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    String ssid = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
    String pass = request->hasParam("password") ? request->getParam("password")->value() : "";
    ssid.trim();
    if (ssid.isEmpty() || ssid.length() > 32) {
      request->send(400, "application/json", "{\"ok\":false,\"err\":\"ssid must be 1-32 chars\"}");
      return;
    }
    if (!pass.isEmpty() && pass.length() < 8) {
      request->send(400, "application/json", "{\"ok\":false,\"err\":\"password must be empty or >= 8 chars\"}");
      return;
    }
    wifiSetCredentials(ssid, pass);
    request->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    requestReboot(); });

  // Soft-AP channel. POST /api/channel?channel=1..13 — saved to NVS, reboot
  // to apply (the AP restarts on the new channel; stations reconnect by SSID).
  server.on("/api/channel", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    int ch = request->hasParam("channel") ? request->getParam("channel")->value().toInt() : 0;
    if (ch < 1 || ch > AP_COUNTRY_NCHAN) {
      request->send(400, "application/json", "{\"ok\":false,\"err\":\"channel out of range\"}");
      return;
    }
    wifiSetChannel((uint8_t)ch);
    request->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    requestReboot(); });

  // 2.4 GHz band scan, for picking a quiet channel.
  //   POST /api/scan  → start an async scan (needs the STA interface: AP+STA
  //                     for ~2 s, stations see a brief hiccup — on demand only)
  //   GET  /api/scan  → {state: scanning|done|idle, channel, networks:[{ssid,rssi,ch}]}
  server.on("/api/scan", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!s_wifi_on) {
      request->send(409, "application/json", "{\"ok\":false,\"err\":\"WiFi bridge is off\"}");
      return;
    }
    if (s_scan_running && WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      request->send(200, "application/json", "{\"ok\":true,\"state\":\"scanning\"}");
      return;
    }
    crumb::set(crumb::WEB_SCAN);
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP_STA);
    // async, include hidden SSIDs (they occupy airtime too), active scan,
    // 120 ms per channel (~1.6 s for 13 channels)
    int r = WiFi.scanNetworks(true, true, false, 120);
    s_scan_running = (r == WIFI_SCAN_RUNNING);
    if (!s_scan_running) {
      if (!wifiManagerStaConfigured()) WiFi.mode(WIFI_AP);
      request->send(500, "application/json", "{\"ok\":false,\"err\":\"scan failed to start\"}");
      return;
    }
    DEBUG_WIFI("band scan started");
    request->send(200, "application/json", "{\"ok\":true,\"state\":\"scanning\"}"); });

  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["channel"] = wifiApChannel();
    doc["maxChannel"] = AP_COUNTRY_NCHAN;
    int n = WiFi.scanComplete();
    if (!s_scan_running && n < 0) {
      doc["state"] = "idle";
    } else if (n == WIFI_SCAN_RUNNING) {
      doc["state"] = "scanning";
    } else if (n < 0) {
      doc["state"] = "idle";
      s_scan_running = false;
    } else {
      doc["state"] = "done";
      JsonArray nets = doc["networks"].to<JsonArray>();
      for (int i = 0; i < n; i++) {
        JsonObject o = nets.add<JsonObject>();
        String ssid = WiFi.SSID(i);
        o["ssid"] = ssid.isEmpty() ? "(hidden)" : ssid;
        o["rssi"] = WiFi.RSSI(i);
        o["ch"] = WiFi.channel(i);
      }
      if (s_scan_running) {
        // First read-out after completion: release the STA interface (unless
        // the Home WiFi bridge is using it).
        s_scan_running = false;
        if (!wifiManagerStaConfigured()) WiFi.mode(WIFI_AP);
        DEBUG_WIFI("band scan done: %d networks", n);
      }
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response); });


  // ===========================================================================
  // Data logger (see include/logger.h)
  // ===========================================================================

  // GET /api/log/config -> {channels:[ch,...], rate, max}
  server.on("/api/log/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    uint16_t chs[logger::MAX_CHANNELS];
    size_t n = logger::channels(chs, logger::MAX_CHANNELS);
    // Bare indices only: the page already holds the catalogue, and a full
    // 384-entry list with names would be a ~40 KB string on the heap.
    JsonArray arr = doc["channels"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) arr.add(chs[i]);
    doc["rate"] = logger::rate_hz();
    doc["max"] = logger::MAX_CHANNELS;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out); });

  // POST /api/log/config?channels=12,15,218&rate=10   (channels = ECU indices)
  server.on("/api/log/config", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (logger::running()) {
      request->send(409, "application/json", "{\"ok\":false,\"err\":\"stop the log first\"}");
      return;
    }
    if (request->hasParam("rate")) logger::set_rate_hz((uint8_t)request->getParam("rate")->value().toInt());
    if (request->hasParam("channels")) {
      String csv = request->getParam("channels")->value();
      uint16_t chs[logger::MAX_CHANNELS]; size_t n = 0;
      int start = 0;
      while (start < (int)csv.length()) {
        int comma = csv.indexOf(',', start);
        if (comma < 0) comma = csv.length();
        String tok = csv.substring(start, comma); tok.trim();
        if (tok.length()) {
          if (n >= logger::MAX_CHANNELS) {
            request->send(400, "application/json", "{\"ok\":false,\"err\":\"too many channels\"}");
            return;
          }
          chs[n++] = (uint16_t)tok.toInt();
        }
        start = comma + 1;
      }
      if (!logger::set_channels(chs, n)) {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"bad channel list\"}");
        return;
      }
    }
    request->send(200, "application/json", "{\"ok\":true}"); });

  // POST /api/log/start[?name=track_day]  -> {ok, file}
  server.on("/api/log/start", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    char name[32] = {0};
    String want = request->hasParam("name") ? request->getParam("name")->value() : "";
    if (!logger::start(want.c_str(), name, sizeof(name))) {
      const char *why = logger::running() ? "already running" : "no channels selected or filesystem error";
      request->send(409, "application/json", String("{\"ok\":false,\"err\":\"") + why + "\"}");
      return;
    }
    request->send(200, "application/json", String("{\"ok\":true,\"file\":\"") + name + "\"}"); });

  server.on("/api/log/stop", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    logger::stop();
    request->send(200, "application/json", "{\"ok\":true}"); });

  // GET /api/log/status -> running state + file list
  server.on("/api/log/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    logger::Status st = logger::status();
    doc["running"] = st.running;
    doc["file"] = st.file;
    doc["rows"] = st.rows;
    doc["bytes"] = st.bytes;
    doc["elapsedMs"] = st.elapsed_ms;
    doc["dropped"] = st.dropped;
    doc["fsUsed"] = st.fs_used;
    doc["fsTotal"] = st.fs_total;
    JsonArray files = doc["files"].to<JsonArray>();
    logger::list([](const char *name, size_t bytes, void *ctx) {
      JsonObject o = static_cast<JsonArray *>(ctx)->add<JsonObject>();
      o["name"] = name; o["bytes"] = bytes;
    }, &files);
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out); });

  // POST /api/log/clear -> delete every stored log (not one being written)
  server.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    size_t n = logger::remove_all();
    request->send(200, "application/json", String("{\"ok\":true,\"removed\":") + n + "}"); });

  // POST /api/log/delete?name=log_0003.ilg
  server.on("/api/log/delete", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    String name = request->hasParam("name") ? request->getParam("name")->value() : "";
    bool ok = logger::remove(name.c_str());
    request->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}"); });

  // GET /api/log/sample -> one row of the configured channels, for browser-side
  // logging: {t, ch:[..], v:[..]} (engineering units, registry scaling).
  server.on("/api/log/sample", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    uint16_t chs[logger::MAX_CHANNELS];
    size_t n = logger::channels(chs, logger::MAX_CHANNELS);
    doc["t"] = millis();
    doc["link"] = ecu::g_telemetry.snapshot().link_ok;
    JsonArray c = doc["ch"].to<JsonArray>();
    JsonArray v = doc["v"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
      c.add(chs[i]);
      uint16_t raw = 0;
      int gi = gauges::index_of_channel(chs[i]);
      if (gauges::raw_channel(chs[i], raw) && gi >= 0) {
        const gauges::GaugeDef &d = gauges::def(gi);
        float val = d.is_signed ? (float)(int16_t)raw * d.scale + d.bias : (float)raw * d.scale + d.bias;
        v.add(val);
      } else {
        v.add(nullptr);
      }
    }
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out); });

  // GET /api/log/download?name=log_0003.ilg[&fmt=csv|raw]
  // CSV is produced on the fly from the binary rows using the gauge registry's
  // scaling, one line per row, streamed in chunks so any log size fits in RAM.
  server.on("/api/log/download", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String name = request->hasParam("name") ? request->getParam("name")->value() : "";
    if (name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
      request->send(400, "text/plain", "bad name"); return;
    }
    String path = String(logger::kDir) + "/" + name;
    if (!LittleFS.exists(path)) { request->send(404, "text/plain", "no such log"); return; }
    String fmt = request->hasParam("fmt") ? request->getParam("fmt")->value() : "csv";
    if (fmt != "csv") {
      AsyncWebServerResponse *r = request->beginResponse(LittleFS, path, "application/octet-stream", true);
      request->send(r);
      return;
    }

    struct Csv {
      File f;
      uint16_t nch = 0;
      uint16_t chs[logger::MAX_CHANNELS];
      uint8_t rate = 0;
      bool header_done = false;
      bool eof = false;
      char line[24 + 16 * logger::MAX_CHANNELS];   // one formatted row / header
      size_t line_len = 0, line_off = 0;           // pending bytes not yet sent
    };
    auto st = std::make_shared<Csv>();
    st->f = LittleFS.open(path, FILE_READ);
    uint8_t hdr[12];
    if (!st->f || st->f.read(hdr, 12) != 12 || memcmp(hdr, "IGLG", 4) != 0) {
      request->send(500, "text/plain", "not an IgnitronUSB log"); return;
    }
    st->rate = hdr[5];
    st->nch = hdr[6] | (hdr[7] << 8);
    if (st->nch > logger::MAX_CHANNELS) { request->send(500, "text/plain", "bad header"); return; }
    for (uint16_t i = 0; i < st->nch; i++) {
      uint8_t b[2]; if (st->f.read(b, 2) != 2) { request->send(500, "text/plain", "short header"); return; }
      st->chs[i] = b[0] | (b[1] << 8);
    }

    AsyncWebServerResponse *res = request->beginChunkedResponse("text/csv",
      [st](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        size_t out = 0;
        for (;;) {
          // flush pending line bytes first
          if (st->line_off < st->line_len) {
            size_t n = st->line_len - st->line_off;
            if (n > maxLen - out) n = maxLen - out;
            memcpy(buf + out, st->line + st->line_off, n);
            st->line_off += n; out += n;
            if (out == maxLen) return out;
          }
          if (st->eof) return out;   // 0 here = end of stream
          // build the next line
          st->line_len = 0; st->line_off = 0;
          if (!st->header_done) {
            st->header_done = true;
            size_t L = 0;
            L += snprintf(st->line + L, sizeof(st->line) - L, "time_s");
            for (uint16_t i = 0; i < st->nch && L < sizeof(st->line) - 2; i++) {
              int gi = gauges::index_of_channel(st->chs[i]);
              if (gi >= 0) {
                const gauges::GaugeDef &d = gauges::def(gi);
                L += snprintf(st->line + L, sizeof(st->line) - L, ",%s%s%s%s", d.label,
                              d.unit[0] ? " (" : "", d.unit, d.unit[0] ? ")" : "");
              } else {
                L += snprintf(st->line + L, sizeof(st->line) - L, ",ch%u", st->chs[i]);
              }
            }
            L += snprintf(st->line + L, sizeof(st->line) - L, "\n");
            st->line_len = L;
            continue;
          }
          uint8_t row[4 + 2 * logger::MAX_CHANNELS];
          size_t need = 4 + 2 * st->nch;
          if (st->f.read(row, need) != (int)need) { st->eof = true; st->f.close(); continue; }
          uint32_t t = row[0] | (row[1] << 8) | (row[2] << 16) | ((uint32_t)row[3] << 24);
          size_t L = snprintf(st->line, sizeof(st->line), "%lu.%03lu", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
          for (uint16_t i = 0; i < st->nch && L < sizeof(st->line) - 2; i++) {
            uint16_t raw = row[4 + 2 * i] | (row[5 + 2 * i] << 8);
            int gi = gauges::index_of_channel(st->chs[i]);
            if (gi >= 0) {
              const gauges::GaugeDef &d = gauges::def(gi);
              float v = d.is_signed ? (float)(int16_t)raw * d.scale + d.bias : (float)raw * d.scale + d.bias;
              L += snprintf(st->line + L, sizeof(st->line) - L, ",%.*f", d.dp, (double)v);
            } else {
              L += snprintf(st->line + L, sizeof(st->line) - L, ",%u", raw);
            }
          }
          L += snprintf(st->line + L, sizeof(st->line) - L, "\n");
          st->line_len = L;
        }
      });
    String csvname = name; csvname.replace(".ilg", ".csv");
    res->addHeader("Content-Disposition", String("attachment; filename=\"") + csvname + "\"");
    request->send(res); });

  // ===========================================================================
  // Parameters: export / import of the user's dashboard + logging choices
  // ===========================================================================
  server.on("/api/params", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["format"] = "ignitronusb-params";
    doc["version"] = 1;
    doc["fw"] = FW_VERSION;
    JsonObject g = doc["dashboard"].to<JsonObject>();
    JsonArray en = g["enabled"].to<JsonArray>();
    for (size_t i = 0; i < gauges::count(); i++) if (gauges::enabled(i)) en.add(gauges::def(i).key);
    g["hero"] = gauges::hero_key();
    g["order"] = gauges::order();
    g["arrangeByType"] = gauges::arrange_by_type();
    g["sweep"] = gauges::sweep_on_launch();
    g["autoLaunch"] = gauges::auto_launch();
    JsonObject lg = doc["logging"].to<JsonObject>();
    JsonArray lc = lg["channels"].to<JsonArray>();
    uint16_t chs[logger::MAX_CHANNELS];
    size_t n = logger::channels(chs, logger::MAX_CHANNELS);
    for (size_t i = 0; i < n; i++) {
      int gi = gauges::index_of_channel(chs[i]);
      lc.add(gi >= 0 ? String(gauges::def(gi).key) : String("ch") + chs[i]);
    }
    lg["rate"] = logger::rate_hz();
    String out; serializeJsonPretty(doc, out);
    AsyncWebServerResponse *res = request->beginResponse(200, "application/json", out);
    res->addHeader("Content-Disposition", "attachment; filename=\"ignitronusb-params.json\"");
    request->send(res); });

  // POST /api/params with the JSON body from an export.
  server.on("/api/params", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      String *body = static_cast<String *>(request->_tempObject);
      if (!body) { request->send(400, "application/json", "{\"ok\":false,\"err\":\"no body\"}"); return; }
      JsonDocument doc;
      DeserializationError e = deserializeJson(doc, *body);
      delete body; request->_tempObject = nullptr;
      if (e || doc["format"] != "ignitronusb-params") {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"not an IgnitronUSB parameters file\"}");
        return;
      }
      int applied = 0, skipped = 0;
      JsonObject g = doc["dashboard"];
      if (!g.isNull()) {
        if (g["enabled"].is<JsonArray>()) {
          gauges::clear_enabled();
          for (JsonVariant k : g["enabled"].as<JsonArray>()) {
            int idx = gauges::index_of(k.as<const char *>());
            if (idx >= 0 && gauges::set_enabled((size_t)idx, true)) applied++; else skipped++;
          }
        }
        if (g["arrangeByType"].is<bool>()) gauges::set_arrange_by_type(g["arrangeByType"]);
        if (g["sweep"].is<bool>()) gauges::set_sweep_on_launch(g["sweep"]);
        if (g["autoLaunch"].is<bool>()) gauges::set_auto_launch(g["autoLaunch"]);
        if (g["order"].is<const char *>()) gauges::set_order(g["order"]);
        if (g["hero"].is<const char *>()) gauges::set_hero_key(g["hero"]);
      }
      JsonObject lg = doc["logging"];
      if (!lg.isNull() && !logger::running()) {
        if (lg["channels"].is<JsonArray>()) {
          uint16_t chs[logger::MAX_CHANNELS]; size_t n = 0;
          for (JsonVariant k : lg["channels"].as<JsonArray>()) {
            if (n >= logger::MAX_CHANNELS) break;
            const char *key = k.as<const char *>();
            if (!key) continue;
            int idx = gauges::index_of(key);
            if (idx >= 0 && gauges::def(idx).ch != gauges::CH_NOT_SENT) chs[n++] = gauges::def(idx).ch;
            else if (strncmp(key, "ch", 2) == 0) chs[n++] = (uint16_t)atoi(key + 2);
            else skipped++;
          }
          logger::set_channels(chs, n);
        }
        if (lg["rate"].is<int>()) logger::set_rate_hz((uint8_t)lg["rate"].as<int>());
      }
      String out = String("{\"ok\":true,\"applied\":") + applied + ",\"skipped\":" + skipped + "}";
      request->send(200, "application/json", out);
    },
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 32 * 1024) return;                        // oversized: ignored, handler sees no body
      if (index == 0) { delete static_cast<String *>(request->_tempObject); request->_tempObject = new String(); static_cast<String *>(request->_tempObject)->reserve(total); }
      String *body = static_cast<String *>(request->_tempObject);
      if (body) body->concat((const char *)data, len);
    });

  // Restore factory defaults (open AP, no password) and reboot.
  server.on("/api/factory", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    wifiFactoryReset();
    request->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    requestReboot(); });
}

void setupUI()
{
  // Shared wifi_manager in "project-managed AP" mode: it mounts the web UI
  // filesystem through the OTA guard (a half-written image never reaches lfs),
  // serves "/" (UI, or a recovery page with the two uploads when the filesystem
  // holds no usable UI), static files with no-cache revalidation, and the Home
  // WiFi STA side. The soft-AP itself stays with wifiBridgeStart()/Stop():
  // configurable SSID/channel, GB regulatory domain and the USB/IP throughput
  // tuning have no equivalent in the shared module.
  wifimgr_config_t wcfg = wifiDefaultConfig();
  wcfg.hostName  = AP_SSID;
  wcfg.mdnsName  = "ignitron";  // -> http://ignitron.local
  wcfg.fwVersion = FW_VERSION;  // recovery page only; index.html bakes its own
  wcfg.manageAp  = false;
  // MUST precede wifiManagerInit(): that mounts the web-UI filesystem via
  // otaFsMountSafe(), so ota_manager has to be configured first or a failed
  // mount passes silently.
  ota_config_t ocfg = otaDefaultConfig();
  ocfg.fwVersion  = FW_VERSION;
  ocfg.product    = "Ignitron USB";
  ocfg.githubRepo = "adamforbes92/usb2ip"; // Releases/ + releases.json for "Check for updates"
  ocfg.verbose    = true;       // routed through the [OTA] serial tag
  otaManagerInit(&ocfg);

  wifiManagerInit(&wcfg);
  if (otaFsMounted())
  {
    logger::begin();
    DEBUG_WEB("LittleFS mounted (%u/%u KB used)",
              (unsigned)(LittleFS.usedBytes() / 1024),
              (unsigned)(LittleFS.totalBytes() / 1024));
  }
  else
  {
    DEBUG_WEB("web UI filesystem not usable - recovery page will be served at /");
  }

  setupWebRoutes();

  // Raw USB capture stream for the Diagnostics tab (see capWsEvent above).
  s_capWs.onEvent(capWsEvent);
  server.addHandler(&s_capWs);

  // "/" + static files (must come after our own routes).
  wifiManagerAttachStatic(server);

  server.begin();
  DEBUG_WEB("gauge UI on http://%s:%u", WiFi.softAPIP().toString().c_str(), WEB_HTTP_PORT);
}
