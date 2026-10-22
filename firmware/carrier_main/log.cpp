#include "log.h"

#include <Arduino.h>
#include <stdarg.h>

#include "config.h"

static volatile bool muted;

void logMute(bool m) { muted = m; }

void logMsg(const char *fmt, ...) {
  char buf[200];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  if (!muted) HOST_UART.println(buf);
  if (Serial) Serial.println(buf);
}
