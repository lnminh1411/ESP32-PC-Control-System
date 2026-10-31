// Wi-Fi station and the small HTTP server.
//
// Joins WIFI_SSID (settings.h) in the background and keeps reconnecting; the
// rest of the firmware runs whether or not the network is up. Once
// connected: mDNS name NET_HOSTNAME.local and, when OTA_PASS is set, an
// HTTP server on port 80 (PUT /layout with header X-OTA-Pass, GET /bench).
// Firmware updates go over USB only (scripts/idf_flash.py): there is no OTA.
#pragma once
#include <stdint.h>

void netInit();
void netPoll();
void netScan();                 // console `wifi scan`: log the networks the board hears
bool netUp();
const char *netIp();            // "" while down
int netRssi();                  // dBm, 0 while down
// A network answer came back (Discord heartbeat ACK, an HTTP reply). With
// Wi-Fi up but nothing for NET_SILENT_MS, netPoll rejoins the network.
void netNoteTraffic();
