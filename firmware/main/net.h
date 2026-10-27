// Wi-Fi station + over-the-air updates.
//
// Joins WIFI_SSID (secrets.h) in the background and keeps reconnecting; the
// rest of the firmware runs whether or not the network is up. Once
// connected: mDNS name NET_HOSTNAME.local and, when OTA_PASS is set, an
// HTTP server endpoint for OTA updates on port 80 (POST /ota with header
// X-OTA-Pass). OTA matters here: once the board is in the case there is
// no physical access to BOOT/RESET (docs/recovery-and-physical-access.md).
#pragma once
#include <stdint.h>

void netInit();
void netPoll();
void netScan();                 // console `wifi scan`: log the networks the board hears                 // call from loop(): OTA + connection logging
bool netUp();
const char *netIp();            // "" while down
int netRssi();                  // dBm, 0 while down
// A network answer came back (Discord heartbeat ACK, an HTTP reply). With
// Wi-Fi up but nothing for NET_SILENT_MS, netPoll rejoins the network.
void netNoteTraffic();
