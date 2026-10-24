#include "wol.h"

#include <arpa/inet.h>
#include <esp_netif.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "pcstats.h"
#include "sys.h"

static uint8_t s_mac[6];
static bool s_macValid = false;
static char s_macStr[18] = "";

static bool parseMac(const char *str, uint8_t mac[6]) {
  if (!str) return false;
  int values[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) == 6 ||
      sscanf(str, "%x-%x-%x-%x-%x-%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) == 6) {
    bool allZero = true;
    bool allFF = true;
    for (int i = 0; i < 6; i++) {
      if (values[i] < 0 || values[i] > 255) return false;
      mac[i] = (uint8_t)values[i];
      if (mac[i] != 0x00) allZero = false;
      if (mac[i] != 0xFF) allFF = false;
    }
    return !allZero && !allFF;
  }
  return false;
}

static void saveMacToNvs(const uint8_t mac[6]) {
  nvs_handle_t h;
  if (nvs_open("wol", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, "mac", mac, 6);
    nvs_commit(h);
    nvs_close(h);
  }
}

void wolInit() {
  nvs_handle_t h;
  if (nvs_open("wol", NVS_READONLY, &h) == ESP_OK) {
    size_t len = 6;
    uint8_t blob[6];
    if (nvs_get_blob(h, "mac", blob, &len) == ESP_OK && len == 6) {
      memcpy(s_mac, blob, 6);
      s_macValid = true;
      snprintf(s_macStr, sizeof(s_macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    }
    nvs_close(h);
  }
}

void wolPoll() {
  static uint32_t lastCheckMs = 0;
  uint32_t now = nowMs();
  if (now - lastCheckMs < 1000) return;
  lastCheckMs = now;

  const char *macText = pcText("pc.mac");
  if (!macText || !*macText) return;

  uint8_t newMac[6];
  if (!parseMac(macText, newMac)) return;

  if (!s_macValid || memcmp(s_mac, newMac, 6) != 0) {
    memcpy(s_mac, newMac, 6);
    s_macValid = true;
    snprintf(s_macStr, sizeof(s_macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    saveMacToNvs(s_mac);
    const char *ifName = pcText("pc.mac_if");
    logMsg("wol: PC MAC %s (from %s)", s_macStr, (ifName && *ifName) ? ifName : "unknown");
  }
}

bool wolSetMac(const char *s) {
  uint8_t mac[6];
  if (!parseMac(s, mac)) return false;
  memcpy(s_mac, mac, 6);
  s_macValid = true;
  snprintf(s_macStr, sizeof(s_macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
  saveMacToNvs(s_mac);
  logMsg("wol: MAC set to %s", s_macStr);
  return true;
}

const char *wolMacStr() {
  return s_macValid ? s_macStr : "";
}

const char *wolSend() {
  if (!netUp()) return "network down";
  if (!s_macValid) return "no MAC known";

  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) {
    memcpy(packet + 6 + i * 6, s_mac, 6);
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) return "socket failed";

  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) netif = esp_netif_next(nullptr);
  esp_netif_ip_info_t ip_info;
  uint32_t bcast_addr = 0;
  bool has_bcast = false;
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    bcast_addr = ip_info.ip.addr | ~ip_info.netmask.addr;
    has_bcast = true;
  }

  for (int iter = 0; iter < 3; iter++) {
    if (iter > 0) sleepMs(100);

    // 1. 255.255.255.255:WOL_PORT
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(WOL_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest, sizeof(dest));

    // 2. Subnet broadcast on WOL_PORT and port 7
    if (has_bcast) {
      dest.sin_addr.s_addr = bcast_addr;
      dest.sin_port = htons(WOL_PORT);
      sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest, sizeof(dest));

      dest.sin_port = htons(7);
      sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
    }
  }

  close(sock);
  logMsg("wol: magic packet sent to %s", s_macStr);
  return nullptr;
}
