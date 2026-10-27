#include "pushover.h"

#include <ctype.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "settings_or_example.h"

static bool errLogged;

static void urlEncode(const char *src, char *dst, size_t dstSize) {
  size_t di = 0;
  for (size_t si = 0; src && src[si] && di + 4 < dstSize; si++) {
    char c = src[si];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[di++] = c;
    } else if (c == ' ') {
      dst[di++] = '+';
    } else {
      di += snprintf(dst + di, dstSize - di, "%%%02X", (unsigned char)c);
    }
  }
  dst[di] = '\0';
}

void pushoverInit() {
  errLogged = false;
}

bool pushoverSend(const char *title, const char *message) {
  if (!PUSHOVER_TOKEN[0] || !PUSHOVER_USER[0] || !message || !message[0]) {
    return false;
  }

  char encTitle[96];
  char encMsg[384];
  urlEncode(title ? title : "Rain soon", encTitle, sizeof(encTitle));
  urlEncode(message, encMsg, sizeof(encMsg));

  char body[768];
  int bodyLen = snprintf(body, sizeof(body), "token=%s&user=%s&title=%s&message=%s",
                         PUSHOVER_TOKEN, PUSHOVER_USER, encTitle, encMsg);

  esp_http_client_config_t cfg = {};
  cfg.url = "https://api.pushover.net/1/messages.json";
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.method = HTTP_METHOD_POST;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    if (!errLogged) {
      errLogged = true;
      logMsg("pushover: client init failed");
    }
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  esp_http_client_set_post_field(client, body, bodyLen);

  esp_err_t err = esp_http_client_perform(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err == ESP_OK && code >= 200 && code < 300) {
    errLogged = false;
    logMsg("pushover: message sent");
    return true;
  }

  if (!errLogged) {
    errLogged = true;
    logMsg("pushover: send failed (err %s, code %d)", esp_err_to_name(err), code);
  }
  return false;
}
