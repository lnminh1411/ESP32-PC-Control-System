// Wi-Fi station + over-the-air updates.
//
// Joins WIFI_SSID (secrets.h) in the background and keeps reconnecting; the
// rest of the firmware runs whether or not the network is up. Once
// connected: mDNS name NET_HOSTNAME.local and, when OTA_PASS is set, an
// ArduinoOTA endpoint (upload with arduino-cli --port <ip> / the IDE's
// network port). OTA matters here: once the board is in the case there is
// no physical access to BOOT/RESET (docs/recovery-and-physical-access.md).
#pragma once
#include <stdint.h>

void netInit();
void netPoll();                 // call from loop(): OTA + connection logging
bool netUp();
const char *netIp();            // "" while down
int netRssi();                  // dBm, 0 while down
