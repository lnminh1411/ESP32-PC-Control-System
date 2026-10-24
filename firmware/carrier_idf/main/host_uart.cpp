#include "host_uart.h"

#include <driver/uart.h>
#include <driver/uart_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static int s_peek = -1;

void hostUartBegin() {
  const uart_config_t cfg = {
    .baud_rate = UART_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 122,
    .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 32768, 4096, 0, NULL, 0));
  uart_vfs_dev_use_driver(UART_NUM_0);
}

int hostUartAvailable() {
  size_t len = 0;
  uart_get_buffered_data_len(UART_NUM_0, &len);
  return (int)len + (s_peek != -1 ? 1 : 0);
}

int hostUartPeek() {
  if (s_peek != -1) return s_peek;
  uint8_t b;
  int n = uart_read_bytes(UART_NUM_0, &b, 1, 0);
  if (n == 1) {
    s_peek = b;
    return s_peek;
  }
  return -1;
}

int hostUartRead() {
  if (s_peek != -1) {
    int c = s_peek;
    s_peek = -1;
    return c;
  }
  uint8_t b;
  int n = uart_read_bytes(UART_NUM_0, &b, 1, 0);
  return (n == 1) ? (int)b : -1;
}

size_t hostUartReadBytes(void *buf, size_t len, uint32_t timeoutMs) {
  if (!buf || len == 0) return 0;
  uint8_t *p = (uint8_t *)buf;
  size_t readCount = 0;

  if (s_peek != -1) {
    *p++ = (uint8_t)s_peek;
    s_peek = -1;
    readCount++;
    len--;
  }

  if (len > 0) {
    TickType_t ticks = timeoutMs ? pdMS_TO_TICKS(timeoutMs) : 0;
    int n = uart_read_bytes(UART_NUM_0, p, len, ticks);
    if (n > 0) readCount += (size_t)n;
  }

  return readCount;
}

void hostUartWrite(const void *data, size_t len) {
  if (data && len > 0) {
    uart_write_bytes(UART_NUM_0, data, len);
  }
}

void hostUartPrint(const char *s) {
  if (s) hostUartWrite(s, strlen(s));
}

void hostUartPrintln(const char *s) {
  if (s) hostUartWrite(s, strlen(s));
  hostUartWrite("\n", 1);
}

void hostUartPrintf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if ((size_t)n < sizeof(buf)) {
    hostUartWrite(buf, n);
  } else {
    char *dyn = (char *)malloc(n + 1);
    if (dyn) {
      va_start(ap, fmt);
      vsnprintf(dyn, n + 1, fmt, ap);
      va_end(ap);
      hostUartWrite(dyn, n);
      free(dyn);
    }
  }
}
