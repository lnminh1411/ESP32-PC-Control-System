// Displays: dirty-rectangle engine, RGB444 on the wire, plus backlights.
//
// Each panel has its own SPI device (own CS) and two PSRAM buffers:
//   canvas -- what the panel should show (Adafruit GFXcanvas16, RGB565),
//             views draw into it freely;
//   sent   -- what the panel shows now.
// displayFlush() diffs the two in 8-row bands, merges runs of dirty bands
// into one rectangle (x range = union, even-aligned), and pushes only those,
// converted to RGB444 (12 bpp, 2 px in 3 bytes: 25% less than RGB565).
// A static dashboard costs nothing; a changed number costs its glyphs.
#pragma once
#include <Adafruit_GFX.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <stdint.h>

#include "config.h"

enum View { VIEW_GIF, VIEW_STATUS, VIEW_STATS };

class PsCanvas : public GFXcanvas16 {   // GFXcanvas16 with its buffer in PSRAM
 public:
  PsCanvas(uint16_t w, uint16_t h);
};

struct Panel {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_handle_t lcd;
  PsCanvas *canvas;
  uint16_t *sent;
  View view;
  uint16_t gifIdx;
  uint32_t dueMs;
  uint32_t frames, pushUs, bytes;   // this 1 s window
  float fps, busMs, kbps;           // last window
};

extern Panel panels[N_PANELS];

void displayInit();                 // reset + init all panels, allocate buffers
uint32_t displayFlush(Panel &p);    // push what changed; returns bus microseconds

// Backlights: a per-panel user setting, forced dark while the PC is idle.
void backlightInit();
void backlightSet(int panel, uint8_t pct);   // 0-100
uint8_t backlightPct(int panel);
void backlightIdle(bool idle);
bool backlightIsIdle();
