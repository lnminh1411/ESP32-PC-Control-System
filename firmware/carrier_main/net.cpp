#include "net.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "log.h"
#include "secrets_or_example.h"

static bool otaStarted;
static char ip[16];

static void onWifiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      snprintf(ip, sizeof ip, "%s", WiFi.localIP().toString().c_str());
      logMsg("wifi: connected to %s, %s, %d dBm", WIFI_SSID, ip, WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (ip[0]) logMsg("wifi: disconnected (reason %u), retrying", info.wifi_sta_disconnected.reason);
      ip[0] = 0;
      break;
    default:
      break;
  }
}

void netInit() {
  if (!strcmp(WIFI_SSID, "your-wifi") || !strcmp(WIFI_PASS, "your-wifi-password")) {
    logMsg("wifi: off -- fill in WIFI_SSID / WIFI_PASS in secrets.h");
    return;
  }
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NET_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                 // modem sleep adds 100+ ms latency to MQTT commands
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logMsg("wifi: joining %s", WIFI_SSID);
}

void netPoll() {
  if (!netUp()) return;
  if (!otaStarted) {
    otaStarted = true;
    MDNS.begin(NET_HOSTNAME);
    if (!OTA_PASS[0]) { logMsg("ota: off (no OTA_PASS)"); return; }
    ArduinoOTA.setHostname(NET_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.setMdnsEnabled(false);     // MDNS already started above
    ArduinoOTA.onStart([] { logMsg("ota: receiving update"); });
    ArduinoOTA.onEnd([] { logMsg("ota: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t err) { logMsg("ota: failed (%u)", err); });
    ArduinoOTA.begin();
    MDNS.enableArduino(3232, true);
    logMsg("ota: ready at %s.local / %s", NET_HOSTNAME, ip);
  }
  if (OTA_PASS[0]) ArduinoOTA.handle();
}

bool netUp() { return ip[0] != 0; }
const char *netIp() { return ip; }
int netRssi() { return netUp() ? WiFi.RSSI() : 0; }
