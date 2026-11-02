#include "console.h"

#include <mbedtls/md.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "host_uart.h"
#include "pcstats.h"
#include "settings_or_example.h"
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

enum Port { P_UART, P_CDC, P_VENDOR, P_HID };

// The helper's link challenge (`@auth <nonce>`, PcStats.cs): the first 16
// bytes of HMAC-SHA256(LINK_KEY, "esp32-link <nonce>") back as `!auth <hex>`,
// on the port it came in on
static void authAnswer(const char *nonce, Port port) {
  if (!LINK_KEY[0] || strlen(nonce) > 64) return;
  char msg[80];
  snprintf(msg, sizeof msg, "esp32-link %s", nonce);
  uint8_t mac[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)LINK_KEY, strlen(LINK_KEY),
                      (const uint8_t *)msg, strlen(msg), mac) != 0) return;
  char line[48] = "!auth ";
  for (int i = 0; i < 16; i++) snprintf(line + 6 + i * 2, 3, "%02x", mac[i]);
  strcat(line, "\n");
  if (port == P_UART) hostUartPrint(line);
  else if (port == P_CDC) usbCdcWrite(line, strlen(line));
  else usbPcSendLine(line);   // vendor pipe while the helper uses it, else HID
}

static void dispatch(char *line, Port port) {   // '@' = PC stats service, else a command
  if (!strncmp(line, "@auth ", 6)) authAnswer(line + 6, port);
  else if (line[0] == '@') pcStatsLine(line);
  else commandRun(line);
}

void consolePoll() {
  if (!lines && !(lines = (LineBuf *)psramCalloc(4 * sizeof(LineBuf)))) return;
  LineBuf &uartLine = lines[0], &usbLine = lines[1], &vendorLine = lines[2], &hidLine = lines[3];
  for (int i = 0; i < POLL_MAX && hostUartAvailable(); i++) {
    if (feed(uartLine, hostUartRead())) dispatch(uartLine.buf, P_UART);
  }
  for (int i = 0; i < POLL_MAX && usbCdcAvailable() > 0; i++) {
    int c = usbCdcRead();
    if (c >= 0) {
      if (feed(usbLine, c)) dispatch(usbLine.buf, P_CDC);
    }
  }
  for (int i = 0; i < POLL_MAX && usbVendorAvailable() > 0; i++) {
    int c = usbVendorRead();
    if (c > 0) {
      if (feed(vendorLine, c)) dispatch(vendorLine.buf, P_VENDOR);
    }
  }
  for (int i = 0; i < POLL_MAX && usbHidAvailable() > 0; i++) {
    int c = usbHidRead();
    if (c > 0) {
      if (feed(hidLine, c)) dispatch(hidLine.buf, P_HID);
    }
  }
}
