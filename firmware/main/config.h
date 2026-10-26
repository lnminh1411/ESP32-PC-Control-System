// Board pins and build-time settings, rev 3.1 carrier.
//
// The pin map below must match pcb/main.ato: scripts/check_pins.py compares
// the two (it parses the PIN_* lines, so keep them one per line).
#pragma once
#include <stdint.h>

// ---- pins (rev 3.1) ----
#define PIN_SCK   12   // shared, via 33R (FSPI IO_MUX pin)
#define PIN_MOSI  11   // shared, via 33R (FSPI IO_MUX pin)
#define PIN_DC    9    // shared
static constexpr int PIN_CS[3]   = {8, 18, 17};
static constexpr int PIN_RST[3]  = {10, 16, 15};
static constexpr int PIN_BL[3]   = {3, 6, 5};
static constexpr int PIN_DOOR[4] = {39, 40, 41, 42};   // keyfob B1-B4 (opto rows 1-4)
#define PIN_MOBO_RST 1                                  // opto row 5
#define PIN_LED      2

// ---- display ----
#define N_PANELS 3
#define TFT_W 240
#define TFT_H 320
#ifndef SPI_HZ
#define SPI_HZ 40000000   // 80 MHz gave blank panels on the carrier bus
#endif
#define CHUNK_LINES 32
#define CHUNK_BYTES (TFT_W * CHUNK_LINES * 2)

#define LED_DUTY_CEIL 0.35f  // green LED glares above ~35% duty; brightness 100 maps here
#define LED_GAMMA     2.2f   // brightness % is perceptual, not linear duty

// ---- optos ----
#define DOOR_PRESS_MS 500
#define DOOR_GAP_MS   500
#define MOBO_RST_MS   200
// Bench only (LED on J6 instead of the motherboard): hold RST HIGH all the
// time. On a real motherboard that keeps the PC in reset, so the build also
// needs -DBENCH_NO_MOTHERBOARD=1 to say J6 is not wired to one.
#ifndef MOBO_RST_HOLD_TEST
#define MOBO_RST_HOLD_TEST 0
#endif
#if MOBO_RST_HOLD_TEST && !BENCH_NO_MOTHERBOARD
#error "MOBO_RST_HOLD_TEST holds the PC in reset: also pass -DBENCH_NO_MOTHERBOARD=1 if J6 only has a test LED"
#endif

// ---- PC power state ----
#define USB_GUARD_MS 30000
#define WOL_PORT 9
#define POWER_ACK_TIMEOUT_MS 3000

// ---- module's own WS2812 (not on the carrier; YD-style boards need the
// "RGB" solder jumper bridged). Held off. ----
#define ONBOARD_RGB_GPIO 48

// ---- network (credentials in secrets.h) ----
#define NET_HOSTNAME "esp32-pc-control"   // mDNS + OTA name

// ---- GIF storage / upload ----
#define UART_BAUD  3000000
#define MAX_FRAMES 64
#define GIF_FILE   "/ffat/gif.bin"  // "GIF5" + w,h,n + durations + frames
