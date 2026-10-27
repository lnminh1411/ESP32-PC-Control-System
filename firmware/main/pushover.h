#pragma once
#include <stdbool.h>

void pushoverInit();
// priority: Pushover's -2..2; -1 = quiet (no sound or vibration), 0 = normal
bool pushoverSend(const char *title, const char *message, int priority = 0);   // blocking HTTPS POST
// Queue a notification for pushoverService (any task, never blocks). An
// empty message sends the title as the message.
void pushoverQueue(const char *title, const char *message, int priority = 0);
// Sends one queued notification; the weather task calls it every second.
void pushoverService();
