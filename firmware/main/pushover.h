#pragma once
#include <stdbool.h>

void pushoverInit();
bool pushoverSend(const char *title, const char *message);   // blocking HTTPS POST
// Queue a notification for pushoverService (any task, never blocks). An
// empty message sends the title as the message.
void pushoverQueue(const char *title, const char *message);
// Sends one queued notification; the weather task calls it every second.
void pushoverService();
