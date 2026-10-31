// Logging: UART always, native USB only while a host is listening.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void logMsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Also copy lines logged by the calling task into buf (NUL-terminated,
// one per line) until logCapture(nullptr, 0): command replies for Discord.
void logCapture(char *buf, unsigned size);

#ifdef __cplusplus
}
#endif
