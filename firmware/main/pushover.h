#pragma once
#include <stdbool.h>

void pushoverInit();
bool pushoverSend(const char *title, const char *message);
