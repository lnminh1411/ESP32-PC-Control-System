#include "log.h"

#include <Arduino.h>
#include <stdarg.h>

#include "config.h"

static volatile bool muted;
static void (*hook)(const char *);
static char *capBuf;
static unsigned capSize, capLen;
static TaskHandle_t capTask;

void logCapture(char *buf, unsigned size) {
  capTask = buf ? xTaskGetCurrentTaskHandle() : nullptr;
  capBuf = buf;
  capSize = size;
  capLen = 0;
  if (buf && size) buf[0] = 0;
}

void logSetHook(void (*h)(const char *)) { hook = h; }

void logMute(bool m) { muted = m; }

void logMsg(const char *fmt, ...) {
  char buf[200];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  if (!muted) HOST_UART.println(buf);
  if (Serial) Serial.println(buf);
  if (hook) hook(buf);
  if (capBuf && capTask == xTaskGetCurrentTaskHandle() && capLen + 2 < capSize) {
    capLen += snprintf(capBuf + capLen, capSize - capLen, "%s\n", buf);
    if (capLen > capSize - 1) capLen = capSize - 1;   // truncated
  }
}
