#include "console.h"

#include <stdint.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "host_uart.h"
#include "pcstats.h"
#include "sys.h"
#include "usb.h"

struct LineBuf { char buf[400]; uint16_t n; };   // PC stats dictionary lines run long

// Bytes per port per pass: a port that outruns the parser would otherwise
// hold the main loop here, past the watchdog reset
#define POLL_MAX 4096

// Line assembly per port: UART, CDC, vendor, HID (1.6 KB, in PSRAM)
static LineBuf *lines;

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

static void dispatch(char *line) {      // '@' = PC stats service, else a command
  if (line[0] == '@') pcStatsLine(line);
  else commandRun(line);
}

void consolePoll() {
  if (!lines && !(lines = (LineBuf *)psramCalloc(4 * sizeof(LineBuf)))) return;
  LineBuf &uartLine = lines[0], &usbLine = lines[1], &vendorLine = lines[2], &hidLine = lines[3];
  for (int i = 0; i < POLL_MAX && hostUartAvailable(); i++) {
    if (feed(uartLine, hostUartRead())) dispatch(uartLine.buf);
  }
  for (int i = 0; i < POLL_MAX && usbCdcAvailable() > 0; i++) {
    int c = usbCdcRead();
    if (c >= 0) {
      if (feed(usbLine, c)) dispatch(usbLine.buf);
    }
  }
  for (int i = 0; i < POLL_MAX && usbVendorAvailable() > 0; i++) {
    int c = usbVendorRead();
    if (c > 0) {
      if (feed(vendorLine, c)) dispatch(vendorLine.buf);
    }
  }
  for (int i = 0; i < POLL_MAX && usbHidAvailable() > 0; i++) {
    int c = usbHidRead();
    if (c > 0) {
      if (feed(hidLine, c)) dispatch(hidLine.buf);
    }
  }
}
