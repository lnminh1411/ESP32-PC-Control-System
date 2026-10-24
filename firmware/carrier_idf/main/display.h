// Displays: core 0 flush pipeline, dirty-rect diff, RGB444 on wire, backlights.
//
// Core 1 draws into canvas or selects GIF frames; core 0 flush task diffs,
// packs and pushes pixels over SPI. ONLY the flush task touches SPI/esp_lcd.
// Each panel has its own SPI device (own CS) and two frame buffers:
//   canvas -- what the panel should show (Adafruit GFXcanvas16, RGB565;
//             internal RAM for status/stats when fitting, PSRAM for GIF);
//   sent   -- what the panel shows now (PSRAM).
// The flush task diffs canvas vs sent in 8-row bands and pushes RGB444
// (12 bpp, 2 px in 3 bytes). GIF panels bypass canvas and copy pre-packed
// RGB444 rows straight to DMA.
#pragma once
#include <Adafruit_GFX.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <stdint.h>

#include "config.h"

enum View { VIEW_GIF, VIEW_STATUS, VIEW_STATS };

class PsCanvas : public GFXcanvas16 {   // GFXcanvas16 with buffer in internal RAM or PSRAM
 public:
  PsCanvas(uint16_t w, uint16_t h, bool internal = false);
  bool isInternal() const { return _isInternal; }

 private:
  bool _isInternal;
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
  volatile bool busy;
  int16_t gifShown;                 // index on glass, -1 = unknown
  uint16_t gifFrame;                // frame index to push
};

extern Panel panels[N_PANELS];

void displayInit();                 // reset + init all panels, start flush task
void displaySubmit(int panel);      // busy=true, queue it; caller must not touch canvas until !busy
void displayWait(int panel);        // block (vTaskDelay(1) loop) until !busy
void displayWaitAll();

// Backlights: a per-panel user setting, forced dark while the PC is idle.
void backlightInit();
void backlightSet(int panel, uint8_t pct);   // 0-100
uint8_t backlightPct(int panel);
void backlightIdle(bool idle);
bool backlightIsIdle();
