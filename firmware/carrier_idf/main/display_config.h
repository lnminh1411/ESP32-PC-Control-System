#pragma once
#include <stdint.h>
#include "config.h"
#include "display.h"
#include "hal/spi_types.h"

struct BusConfig {
  int sck;
  int mosi;
  int dc;
  spi_host_device_t host;
  int hz;
};

struct PanelConfig {
  int cs;
  int rst;
  int bl;
  int w;
  int h;
  uint8_t madctl;
  bool invert;
  int colStart;
  int rowStart;
  View defaultView;
  bool preferInternalCanvas;
};

inline constexpr BusConfig displayBus = {
  PIN_SCK,
  PIN_MOSI,
  PIN_DC,
  SPI2_HOST,
  SPI_HZ,
};

inline constexpr PanelConfig displayPanels[N_PANELS] = {
  { PIN_CS[0], PIN_RST[0], PIN_BL[0], TFT_W, TFT_H, 0, true, 0, 0, VIEW_METRICS, false },
  { PIN_CS[1], PIN_RST[1], PIN_BL[1], TFT_W, TFT_H, 0, true, 0, 0, VIEW_METRICS, true },
  { PIN_CS[2], PIN_RST[2], PIN_BL[2], TFT_W, TFT_H, 0, true, 0, 0, VIEW_METRICS, true },
};

static_assert(sizeof(displayPanels) / sizeof(displayPanels[0]) == N_PANELS,
              "displayPanels count must equal N_PANELS");
