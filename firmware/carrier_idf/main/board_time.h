// Wall clock for the screens. The PC sends pc.epoch (Unix seconds) and
// pc.tz_min (local offset) in every sample; the board keeps counting from the
// last one when the PC is off. SNTP over Wi-Fi takes over when available.
#pragma once
#include <stdbool.h>
#include <time.h>

void boardTimeOnSample();               // pcstats calls it at every @e
bool boardLocalTime(struct tm *out);    // false = no time source yet
