// Logging: UART always, native USB only while a host is listening.
// Muted on the UART during a GIF upload so the upload protocol stays clean.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void logMsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logMute(bool muted);
void logSetHook(void (*hook)(const char *line));   // extra sink (Home Assistant)
// Also copy lines logged by the calling task into buf (NUL-terminated,
// one per line) until logCapture(nullptr, 0): command replies for Discord.
void logCapture(char *buf, unsigned size);

#ifdef __cplusplus
}
#endif
