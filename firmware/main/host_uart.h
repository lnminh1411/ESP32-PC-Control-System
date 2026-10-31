#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hostUartBegin();
int hostUartAvailable();
int hostUartRead();
void hostUartWrite(const void *data, size_t len);
void hostUartPrint(const char *s);
void hostUartPrintln(const char *s);
void hostUartPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
