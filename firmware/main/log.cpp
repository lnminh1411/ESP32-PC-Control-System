#include "log.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "discord.h"
#include "host_uart.h"
#include "usb.h"

static volatile bool s_muted = false;
static char *s_capBuf = nullptr;
static unsigned s_capSize = 0, s_capLen = 0;
static TaskHandle_t s_capTask = nullptr;

void logCapture(char *buf, unsigned size) {
  s_capTask = buf ? xTaskGetCurrentTaskHandle() : nullptr;
  s_capBuf = buf;
  s_capSize = size;
  s_capLen = 0;
  if (buf && size) buf[0] = 0;
}


void logMute(bool m) { s_muted = m; }

static void logEmit(const char *buf, size_t len) {
  discordLog(buf, len);
  if (!s_muted) hostUartPrintln(buf);
  if (usbCdcConnected()) {
    usbCdcWrite(buf, len);
    usbCdcWrite("\n", 1);
  }
  if (s_capBuf && s_capTask == xTaskGetCurrentTaskHandle() && s_capLen + 2 < s_capSize) {
    s_capLen += snprintf(s_capBuf + s_capLen, s_capSize - s_capLen, "%s\n", buf);
    if (s_capLen > s_capSize - 1) s_capLen = s_capSize - 1;
  }
}

void logMsg(const char *fmt, ...) {
  char buf[256];
  va_list a;
  va_start(a, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  if (len <= 0) return;
  if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
  if (len == 0) return;
  logEmit(buf, (size_t)len);
}

static int logVprintf(const char *fmt, va_list a) {
  if (s_muted) return 0;
  static bool inLog = false;
  if (inLog) return 0;
  inLog = true;
  char buf[256];
  int len = vsnprintf(buf, sizeof(buf), fmt, a);
  if (len > 0) {
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    if (len > 0) logEmit(buf, (size_t)len);
  }
  inLog = false;
  return len;
}

__attribute__((constructor))
static void logInitHook() {
  esp_log_set_vprintf(logVprintf);
}
