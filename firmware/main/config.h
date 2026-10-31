// Build-time constants of the rev 3.1 carrier. The pin map, panel geometry
// and timings are settings: settings.h, defaults in settings_or_example.h
// (scripts/check_pins.py compares the pins with pcb/main.ato).
#pragma once
#include <stdint.h>
#include "settings_or_example.h"

// ---- pins (settings.h) ----
static constexpr int PIN_CS[3]   = PIN_CS_LIST;
static constexpr int PIN_RST[3]  = PIN_RST_LIST;
static constexpr int PIN_BL[3]   = PIN_BL_LIST;
static constexpr int PIN_DOOR[4] = PIN_DOOR_LIST;   // keyfob B1-B4 (opto rows 1-4)

// ---- display ----
#define CHUNK_LINES 32
#define CHUNK_BYTES (TFT_W * CHUNK_LINES * 2)

// ---- optos ----
// Bench only (LED on J6 instead of the motherboard): hold RST HIGH all the
// time. On a real motherboard that keeps the PC in reset, so the build also
// needs -DBENCH_NO_MOTHERBOARD=1 to say J6 is not wired to one.
#ifndef MOBO_RST_HOLD_TEST
#define MOBO_RST_HOLD_TEST 0
#endif
#if MOBO_RST_HOLD_TEST && !BENCH_NO_MOTHERBOARD
#error "MOBO_RST_HOLD_TEST holds the PC in reset: also pass -DBENCH_NO_MOTHERBOARD=1 if J6 only has a test LED"
#endif

// ---- GIF storage ----
#define MAX_FRAMES 64
