#include "console.h"

#include <Arduino.h>

#include "commands.h"
#include "config.h"
#include "gif_store.h"
#include "log.h"
#include "views.h"

struct LineBuf { char buf[64]; uint8_t n; };

// Line assembly per port. On the UART an upper-case 'G' at the start of a
// line starts a GIF upload instead (commands are all lower-case).
static LineBuf uartLine, usbLine;

static bool feed(LineBuf &lb, int c) {        // true = a full line is in lb.buf
  if (c == '\r' || c == '\n') {
    if (!lb.n) return false;
    lb.buf[lb.n] = 0;
    lb.n = 0;
    return true;
  }
  if (lb.n < sizeof lb.buf - 1) lb.buf[lb.n++] = (char)c;
  return false;
}

static void waitMagicThenReceive() {
  const char magic[] = "GIF5";
  logMute(true);
  for (;;) {
    uint8_t m = 0;
    uint32_t t0 = millis();
    while (m < 4 && millis() - t0 < 3000) {
      if (HOST_UART.available()) m = (HOST_UART.read() == magic[m]) ? m + 1 : 0;
    }
    if (m < 4) break;                  // not an upload after all
    if (gifReceive(viewsShowGifFrame)) { HOST_UART.println("PLAY"); break; }
  }
  logMute(false);
}

void consolePoll() {
  while (HOST_UART.available()) {
    if (!uartLine.n && HOST_UART.peek() == 'G') { waitMagicThenReceive(); return; }
    if (feed(uartLine, HOST_UART.read())) commandRun(uartLine.buf);
  }
  while (Serial.available())
    if (feed(usbLine, Serial.read())) commandRun(usbLine.buf);
}

void consoleUploadWindow(uint32_t ms) {
  uint32_t t0 = millis();
  while (!HOST_UART.available() && millis() - t0 < ms) delay(5);
  if (HOST_UART.available()) waitMagicThenReceive();
}
