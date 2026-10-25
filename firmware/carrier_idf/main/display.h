// Displays: core 0 flush pipeline, dirty-rect diff, RGB444 on wire, backlights.
//
// Core 1 draws into canvas or selects GIF frames; core 0 flush task diffs,
// packs and pushes pixels over SPI. ONLY the flush task touches SPI/esp_lcd.
// Each panel has its own SPI device (own CS) and two frame buffers:
//   canvas -- what the panel should show (LovyanGFX LGFX_Sprite, RGB565;
//             internal RAM when fitting, PSRAM otherwise);
//   sent   -- what the panel shows now (PSRAM).
// The flush task diffs canvas vs sent in 8-row bands and pushes RGB444
// (12 bpp, 2 px in 3 bytes). GIF panels bypass canvas and copy pre-packed
// RGB444 rows straight to DMA.
#pragma once
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <stdint.h>

#include "config.h"

enum View : uint8_t {
  VIEW_GIF,
  VIEW_STATUS,
  VIEW_STATS,
  VIEW_METRICS,
  VIEW_DASH,
  VIEW_PC,        // screen 1: PC stats, GIF while the PC is off
  VIEW_MEDIA,     // screen 2: media + uptime/network, weather while off
  VIEW_AI,        // screen 3: AI usage / local AI, clock while off
  VIEW_COUNT
};

namespace lgfx {
inline namespace v1 {
class LGFX_Sprite;
}
}
using LGFX_Sprite = lgfx::LGFX_Sprite;

struct Panel {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_handle_t lcd;
  LGFX_Sprite *canvas;
  uint16_t *sent;
  bool canvasInternal;
  View view;
  bool gifMode;                     // flush pushes GIF frames instead of the canvas
  int16_t bandY0, bandY1;           // GIF skips rows [bandY0, bandY1); the canvas fills them
  uint16_t gifIdx;
  uint32_t dueMs;
  uint32_t frames, pushUs, bytes;   // this 1 s window
  float fps, busMs, kbps;           // last window
  volatile bool busy;
  int16_t gifShown;                 // index on glass, -1 = unknown
  uint16_t gifFrame;                // frame index to push

  // Dynamic FPS engine fields
  uint32_t lastFrameBytes;          // bytes sent in the most recent flush
  float emaBytes;                   // EMA of bytes per frame
  uint32_t deferred;                // deferred frames in last 1 s window
  float gifStretch;                 // actual / nominal frame interval last window (GIF only)
};

extern Panel panels[N_PANELS];
extern uint32_t displaySpinUs;

void displayInit();                 // reset + init all panels, start flush task
void displaySubmit(int panel);      // busy=true, queue it; caller must not touch canvas until !busy
void displayWait(int panel);        // block (vTaskDelay(1) loop) until !busy
void displayWaitAll();
float busLoadPct();

// Backlights: a per-panel user setting, forced dark while the PC is idle.
void backlightInit();
void backlightSet(int panel, uint8_t pct);   // 0-100
uint8_t backlightPct(int panel);
void backlightIdle(bool idle);
bool backlightIsIdle();

// Screen setup persistence (NVS namespace "disp")
View displayStoredView(int panel);
void displaySaveView(int panel, View v);
uint8_t displayStoredBacklight(int panel);
void displaySaveBacklight(int panel, uint8_t pct);
void displayNvsPoll();
