#include "console.h"

#include <stdint.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "gif_store.h"
#include "host_uart.h"
#include "log.h"
#include "pcstats.h"
#include "sys.h"
#include "usb.h"
#include "views.h"

struct LineBuf { char buf[400]; uint16_t n; };   // PC stats dictionary lines run long

// Line assembly per port. On the UART an upper-case 'G' at the start of a
// line starts a GIF upload instead (commands are all lower-case).
static LineBuf uartLine, usbLine, hidLine;

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

static void waitMagicThenReceive() {
  const char magic[] = "GIF5";
  logMute(true);
  for (;;) {
    uint8_t m = 0;
    uint32_t t0 = nowMs();
    while (m < 4 && nowMs() - t0 < 3000) {
      if (hostUartAvailable()) m = (hostUartRead() == magic[m]) ? m + 1 : 0;
    }
    if (m < 4) break;                  // not an upload after all
    if (gifReceive(viewsShowGifFrame)) { hostUartPrintln("PLAY"); break; }
  }
  logMute(false);
}

void consolePoll() {
  while (hostUartAvailable()) {
    if (!uartLine.n && hostUartPeek() == 'G') { waitMagicThenReceive(); return; }
    if (feed(uartLine, hostUartRead())) dispatch(uartLine.buf);
  }
  while (usbCdcAvailable() > 0) {
    int c = usbCdcRead();
    if (c >= 0) {
      if (feed(usbLine, c)) dispatch(usbLine.buf);
    }
  }
  while (usbHidAvailable() > 0) {
    int c = usbHidRead();
    if (c > 0) {
      if (feed(hidLine, c)) dispatch(hidLine.buf);
    }
  }
}

void consoleUploadWindow(uint32_t ms) {
  uint32_t t0 = nowMs();
  while (!hostUartAvailable() && nowMs() - t0 < ms) sleepMs(5);
  if (hostUartAvailable()) waitMagicThenReceive();
}
