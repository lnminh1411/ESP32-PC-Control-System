#include "net.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "config.h"
#include "layout.h"
#include "log.h"
#include "settings_or_example.h"
#include "sys.h"

static bool otaStarted;
static char ip[16];
// Watchdogs for a link that stops coming back on its own (netPoll)
static const uint32_t NET_SILENT_MS = 5 * 60 * 1000;    // up but no answers: rejoin
static const uint32_t NET_STUCK_MS = 60 * 1000;         // down and no driver events: connect again
static const uint32_t NET_RESTART_MS = 10 * 60 * 1000;  // down this long: restart the Wi-Fi driver
static volatile uint32_t lastTrafficMs, lastWifiEventMs, downSinceMs;
static httpd_handle_t otaServer;

static bool constantTimeEq(const char *a, const char *b) {
  size_t la = strlen(a);
  size_t lb = strlen(b);
  volatile unsigned char diff = (unsigned char)(la ^ lb);
  size_t maxLen = la > lb ? la : lb;
  for (size_t i = 0; i < maxLen; i++) {
    unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
    unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
    diff |= (unsigned char)(ca ^ cb);
  }
  return diff == 0;
}

static void markAppValidOnce() {
  static bool marked;
  if (marked) return;
  marked = true;
  esp_ota_mark_app_valid_cancel_rollback();
  logMsg("ota: image marked valid");
}

static void rebootTimerCb(void *) {
  esp_restart();
}

static void scheduleReboot(uint32_t delayMs) {
  static esp_timer_handle_t timer;
  if (!timer) {
    esp_timer_create_args_t args = {};
    args.callback = rebootTimerCb;
    args.name = "ota_reboot";
    esp_timer_create(&args, &timer);
  }
  esp_timer_start_once(timer, (uint64_t)delayMs * 1000);
}

static esp_err_t rootGetHandler(httpd_req_t *req) {
  char buf[96];
  snprintf(buf, sizeof buf, "ESP32 PC Control online (%s, uptime %lu s)\n",
           netIp(), (unsigned long)(nowMs() / 1000));
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// An upload that sends nothing this long is dropped: httpd_req_recv times
// out every few seconds and the loops below retry, and a client that stalls
// without closing would otherwise hold the HTTP task (and the OTA handle)
constexpr uint32_t UPLOAD_STALL_MS = 30000;

static esp_err_t otaPostHandler(httpd_req_t *req) {
  char passHdr[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-OTA-Pass", passHdr, sizeof passHdr) != ESP_OK ||
      !constantTimeEq(passHdr, OTA_PASS)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
    return ESP_OK;
  }

  if (req->content_len <= 0) {
    logMsg("ota: failed (%s)", "content length missing");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
    return ESP_OK;
  }

  logMsg("ota: receiving update");

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
  if (!update_partition) {
    logMsg("ota: failed (%s)", "no partition found");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition");
    return ESP_OK;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    logMsg("ota: failed (%s)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return ESP_OK;
  }

  char buf[1024];
  int remaining = req->content_len;
  uint32_t lastDataMs = nowMs();
  while (remaining > 0) {
    int toRead = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int received = httpd_req_recv(req, buf, toRead);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT && nowMs() - lastDataMs < UPLOAD_STALL_MS) continue;
      esp_ota_abort(ota_handle);
      logMsg("ota: failed (%s)", received == HTTPD_SOCK_ERR_TIMEOUT ? "client stalled" : "receive error");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
      return ESP_FAIL;
    }
    lastDataMs = nowMs();
    err = esp_ota_write(ota_handle, buf, received);
    if (err != ESP_OK) {
      esp_ota_abort(ota_handle);
      logMsg("ota: failed (%s)", esp_err_to_name(err));
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
      return ESP_OK;
    }
    remaining -= received;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    logMsg("ota: failed (%s)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
    return ESP_OK;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    logMsg("ota: failed (%s)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
    return ESP_OK;
  }

  logMsg("ota: done, rebooting");
  httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
  scheduleReboot(500);
  return ESP_OK;
}

static esp_err_t layoutPutHandler(httpd_req_t *req) {
  char passHdr[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-OTA-Pass", passHdr, sizeof passHdr) != ESP_OK ||
      !constantTimeEq(passHdr, OTA_PASS)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
    return ESP_OK;
  }

  if (req->content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
    return ESP_OK;
  }

  if (req->content_len > 16384) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large (>16KB)");
    return ESP_OK;
  }

  char *buf = (char *)heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (char *)malloc(req->content_len + 1);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_OK;
  }

  int remaining = req->content_len;
  int receivedTotal = 0;
  uint32_t lastDataMs = nowMs();
  while (remaining > 0) {
    int received = httpd_req_recv(req, buf + receivedTotal, remaining);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT && nowMs() - lastDataMs < UPLOAD_STALL_MS) continue;
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
      return ESP_FAIL;
    }
    lastDataMs = nowMs();
    receivedTotal += received;
    remaining -= received;
  }
  buf[receivedTotal] = '\0';

  const char *err = layoutApply(buf, (size_t)receivedTotal);
  free(buf);

  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    return ESP_OK;
  }

  httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static volatile bool scanning;   // `wifi scan` holds the reconnect off
static bool wifiStarted;         // netInit got as far as connecting

static void wifiEventHandler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
    // a join that keeps failing says why: when the reason changes, else once a minute
    static uint8_t lastReason;
    static uint32_t lastReasonMs;
    uint8_t reason = disconn->reason;
    if (ip[0]) {
      logMsg("wifi: disconnected (reason %u), retrying", reason);
    } else if (reason != lastReason || nowMs() - lastReasonMs >= 60000) {
      const char *why = reason == WIFI_REASON_NO_AP_FOUND ? ", network not seen"
                      : reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                        reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? ", wrong password or security mode"
                      : reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ? ", security mode not supported"
                      : reason == WIFI_REASON_ASSOC_FAIL || reason == WIFI_REASON_ASSOC_EXPIRE ? ", router refused" : "";
      logMsg("wifi: can't join %s (reason %u%s), retrying", WIFI_SSID, reason, why);
      lastReasonMs = nowMs();
    }
    lastReason = reason;
    if (ip[0]) downSinceMs = nowMs();
    ip[0] = 0;
    lastWifiEventMs = nowMs();
    if (!scanning) esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    // the address went away without a disconnect (DHCP lease lost): rejoin
    logMsg("wifi: lost the IP address, rejoining");
    ip[0] = 0;
    downSinceMs = nowMs();
    lastWifiEventMs = downSinceMs;
    esp_wifi_disconnect();   // the disconnect event connects again
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    lastTrafficMs = nowMs();
    lastWifiEventMs = lastTrafficMs;
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&event->ip_info.ip));
    logMsg("wifi: connected to %s, %s, %d dBm", WIFI_SSID, ip, netRssi());
    // netPoll marks the image valid: the otadata write stays off this small
    // event-task stack, and only one task ever runs it
  }
}

void netInit() {
  if (!strcmp(WIFI_SSID, "your-wifi") || !strcmp(WIFI_PASS, "your-wifi-password")) {
    logMsg("wifi: off -- fill in WIFI_SSID / WIFI_PASS in settings.h");
    return;
  }

  esp_netif_init();
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(sta_netif, NET_HOSTNAME);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t e = esp_wifi_init(&cfg);
  if (e != ESP_OK) logMsg("wifi: init failed (%s)", esp_err_to_name(e));

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiEventHandler, nullptr, nullptr);

  esp_wifi_set_mode(WIFI_MODE_STA);

  wifi_config_t wifi_cfg = {};
  strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
  strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

  e = esp_wifi_start();
  if (e != ESP_OK) logMsg("wifi: start failed (%s)", esp_err_to_name(e));
  esp_wifi_set_ps(NET_WIFI_PS);
  // the router is close: a lower TX power cap trims the current peaks on
  // the 5 V the board shares with the PC (units of 0.25 dBm)
  esp_wifi_set_max_tx_power((int8_t)(NET_WIFI_TX_DBM * 4));
  e = esp_wifi_connect();
  if (e != ESP_OK) logMsg("wifi: connect failed (%s)", esp_err_to_name(e));
  downSinceMs = nowMs();
  lastWifiEventMs = downSinceMs;
  wifiStarted = true;
  logMsg("wifi: joining %s", WIFI_SSID);
}

void netNoteTraffic() { lastTrafficMs = nowMs(); }

// Recovery the driver's own retries don't cover: no events at all while
// down, a driver that stays down for many minutes, and an association that
// is up but passes nothing (router hung, lease gone quietly)
static void netWatchdog() {
  if (!wifiStarted || scanning) return;
  uint32_t now = nowMs();
  if (!netUp()) {
    if (now - downSinceMs >= NET_RESTART_MS) {
      logMsg("wifi: down for %u min, restarting the Wi-Fi driver", (unsigned)((now - downSinceMs) / 60000));
      esp_wifi_stop();
      esp_wifi_start();
      esp_wifi_set_ps(WIFI_PS_NONE);
      esp_wifi_set_max_tx_power((int8_t)(NET_WIFI_TX_DBM * 4));
      esp_wifi_connect();
      downSinceMs = now;
      lastWifiEventMs = now;
    } else if (now - lastWifiEventMs >= NET_STUCK_MS) {
      lastWifiEventMs = now;
      esp_wifi_connect();
    }
    return;
  }
  if (now - lastTrafficMs >= NET_SILENT_MS) {
    logMsg("wifi: no answers from the network for %u min, rejoining", (unsigned)((now - lastTrafficMs) / 60000));
    lastTrafficMs = now;
    esp_wifi_disconnect();   // the disconnect event connects again
  }
}

// `wifi scan`: drops the connection attempt, lists every network the board
// hears (hidden ones too), then joins again. Blocks the caller ~3 s.
void netScan() {
  scanning = true;
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(200));
  wifi_scan_config_t sc = {};
  sc.show_hidden = true;
  esp_err_t e = esp_wifi_scan_start(&sc, true);
  if (e != ESP_OK) {
    logMsg("wifi: scan failed (%s)", esp_err_to_name(e));
  } else {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    uint16_t got = n > 30 ? 30 : n;
    wifi_ap_record_t *aps = (wifi_ap_record_t *)heap_caps_calloc(got ? got : 1, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (aps && esp_wifi_scan_get_ap_records(&got, aps) == ESP_OK) {
      logMsg("wifi: %u networks heard (strongest first)", n);
      for (int i = 0; i < got; i++)
        logMsg("  %-32.32s ch %2u %4d dBm auth %d", aps[i].ssid[0] ? (const char *)aps[i].ssid : "(hidden)",
               aps[i].primary, aps[i].rssi, (int)aps[i].authmode);
    } else {
      esp_wifi_clear_ap_list();
    }
    free(aps);
  }
  wifi_country_t c;
  if (esp_wifi_get_country(&c) == ESP_OK)
    logMsg("wifi: country %.2s, channels %u-%u", c.cc, c.schan, c.schan + c.nchan - 1);
  scanning = false;
  esp_wifi_connect();
}

static volatile int rssiCached;

void netPoll() {
  netWatchdog();
  // RSSI read here, on the main loop, every 5 s: asking the Wi-Fi driver from
  // other tasks (the Discord panel) collides with it ("failed to get wifi thread sem")
  static uint32_t lastRssiMs;
  if (netUp() && nowMs() - lastRssiMs >= 5000) {
    lastRssiMs = nowMs();
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssiCached = ap.rssi;
  }
  if (netUp() || nowMs() >= NET_OTA_VALIDATE_MS) {
    markAppValidOnce();
  }
  if (!netUp()) return;
  if (!otaStarted) {
    otaStarted = true;
    mdns_init();
    mdns_hostname_set(NET_HOSTNAME);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    if (!OTA_PASS[0]) {
      logMsg("ota: off (no OTA_PASS)");
      return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.max_open_sockets = 3;   // OTA and layout uploads, one at a time: leave sockets for TLS clients
    if (httpd_start(&otaServer, &config) == ESP_OK) {
      httpd_uri_t get_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = rootGetHandler,
        .user_ctx = nullptr
      };
      httpd_register_uri_handler(otaServer, &get_root);

      httpd_uri_t post_ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = otaPostHandler,
        .user_ctx = nullptr
      };
      httpd_register_uri_handler(otaServer, &post_ota);

      httpd_uri_t put_layout = {
        .uri = "/layout",
        .method = HTTP_PUT,
        .handler = layoutPutHandler,
        .user_ctx = nullptr
      };
      httpd_register_uri_handler(otaServer, &put_layout);

      httpd_uri_t get_bench = {
        .uri = "/bench",
        .method = HTTP_GET,
        .handler = benchHttpHandler,
        .user_ctx = nullptr
      };
      httpd_register_uri_handler(otaServer, &get_bench);

      logMsg("ota: ready at %s.local / %s", NET_HOSTNAME, ip);
    }
  }
}

bool netUp() { return ip[0] != 0; }
const char *netIp() { return ip; }

int netRssi() {
  return netUp() ? rssiCached : 0;
}
