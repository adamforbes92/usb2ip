#ifndef IGNITRON_WIFI_H
#define IGNITRON_WIFI_H

#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// Mount LittleFS, register the web routes and start the async server.
void setupUI(void);
void setupWebRoutes(void);

// WiFi soft-AP ("bridge") power control — toggled by the port state machine.
bool wifiBridgeStart(void);   // bring the soft-AP up (idempotent); true on success
void wifiBridgeStop(void);    // tear the soft-AP down
bool wifiBridgeActive(void);  // is the soft-AP currently up?

// Credential store (NVS). SSID/password are editable from the web UI; an empty
// password means an open network. wifiFactoryReset() wipes creds + settings.
String wifiApSsid(void);
bool wifiApHasPassword(void);
void wifiSetCredentials(const String &ssid, const String &password);
uint8_t wifiApChannel(void);                 // persisted 2.4 GHz channel (default AP_CHANNEL)
void wifiSetChannel(uint8_t channel);        // 1-13, saved to NVS; takes effect on reboot
void wifiFactoryReset(void);
void requestReboot(void);     // schedule a reboot from the main loop
void wifiLoop(void);          // service deferred reboot; call from loop()

#endif  // IGNITRON_WIFI_H
