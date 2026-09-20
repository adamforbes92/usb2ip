#include "tasks.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "defs.h"
#include "usbip_bridge.h"
#include "telemetry.h"
#include "ecu_decoder.h"
#include "ecu_tap.h"
#include "gauges.h"
#include "ignitron_wifi.h"
#include "port_control.h"
#include "crumb.h"

static usbip::USBIPComponent g_bridge;

static const char *resetReasonStr(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:   return "power-on";
  case ESP_RST_EXT:       return "external pin";
  case ESP_RST_SW:        return "software";
  case ESP_RST_PANIC:     return "PANIC (crash)";
  case ESP_RST_INT_WDT:   return "interrupt watchdog";
  case ESP_RST_TASK_WDT:  return "task watchdog";
  case ESP_RST_WDT:       return "other watchdog";
  case ESP_RST_DEEPSLEEP: return "deep sleep";
  case ESP_RST_BROWNOUT:  return "BROWNOUT (voltage sag)";
  case ESP_RST_SDIO:      return "SDIO";
  default:                return "unknown";
  }
}

static void bridgeTask(void *)
{
  for (;;)
  {
    g_bridge.loop();
    crumb::set(crumb::PORT_LOOP);
    port::g_port.loop();
    vTaskDelay(1);
  }
}

static void wifiTask(void *)
{
  for (;;)
  {
    wifiLoop();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#if ECU_MOCK
static void mockEcuTask(void *)
{
  for (;;)
  {
    ecu::mock_tick();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
#endif

static void statusTask(void *)
{
  for (;;)
  {
    crumb::set(crumb::STATUS_TASK);
    ecu::TapStats ts = ecu::tap_stats();
    ecu::TelemetryState st = ecu::g_telemetry.snapshot();
    port::Status ps = port::g_port.status();
    DEBUG("clients=%d heap=%u min=%u blk=%u | ECU link=%d frames=%lu in=%luB out=%luB drop=%lu cap=%d"
          " | usbc=%d ecu5v=%d wifi=%d mux=%d idle=%lus",
          WiFi.softAPgetStationNum(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
          st.link_ok, (unsigned long)st.frame_count,
          (unsigned long)ts.in_bytes, (unsigned long)ts.out_bytes,
          (unsigned long)ts.dropped, ts.client_connected,
          ps.usbc_present, ps.ecu_powered, ps.wifi_on, (int)ps.mux,
          (unsigned long)(ps.idle_remaining_ms / 1000));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startTasks()
{
  DEBUG("IgnitronUSB v%s booting", FW_VERSION);
  DEBUG("last reset: %s", resetReasonStr(esp_reset_reason()));
  crumb::report_after_reset();

  bool ap_ok = wifiBridgeStart();
  IPAddress ip = WiFi.softAPIP();
  if (!ap_ok)
  {
    DEBUG_WIFI("soft-AP start FAILED");
  }

  DEBUG_USB("starting USB host + USB/IP server on port %d", USBIP_TCP_PORT);
  g_bridge.set_port(USBIP_TCP_PORT);
  g_bridge.setup();

  ecu::begin();
  gauges::begin();
  ecu::tap_begin(USBIP_TAP_PORT);
  setupUI();
  port::g_port.begin();

  DEBUG("ready — join \"%s\", then: usbip attach -r %s -b 1-1",
        AP_SSID, ip.toString().c_str());

  xTaskCreatePinnedToCore(bridgeTask, "usbip", 8192, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(wifiTask, "wifi", 3072, nullptr, 1, nullptr, 0);
#if ECU_MOCK
  xTaskCreatePinnedToCore(mockEcuTask, "ecuMock", 3072, nullptr, 1, nullptr, 0);
#endif
  xTaskCreatePinnedToCore(statusTask, "status", 4096, nullptr, 1, nullptr, 0);
}