// Logging: UART always, native USB only while a host is listening.
// Muted on the UART during a GIF upload so the upload protocol stays clean.
#pragma once

void logMsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logMute(bool muted);
