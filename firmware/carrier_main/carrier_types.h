// Types used in function signatures live here: the Arduino builder puts its
// auto-generated prototypes above the sketch body, before any type the .ino
// itself declares.
#pragma once
#include <stdint.h>
#include <Adafruit_GFX.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp32-hal-psram.h>

enum PcState { PC_UNKNOWN, PC_ON, PC_OFF };
enum LedMode { LED_AUTO, LED_ON, LED_OFF };
struct LineBuf { char buf[64]; uint8_t n; };
enum View { VIEW_GIF, VIEW_STATUS, VIEW_STATS };

class PsCanvas : public GFXcanvas16 {          // GFXcanvas16 with its buffer in PSRAM
 public:
  PsCanvas(uint16_t w, uint16_t h) : GFXcanvas16(w, h, false) {
    buffer = (uint16_t *)ps_calloc((size_t)w * h, 2);
  }
};

struct Panel {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_handle_t lcd;
  PsCanvas *canvas;
  uint16_t *sent;
  View view;
  uint16_t gifIdx;
  uint32_t dueMs;
  uint32_t frames, pushUs, bytes;               // this 1 s window
  float fps, busMs, kbps;                       // last window
};
