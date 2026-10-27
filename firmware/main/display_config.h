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

// per panel, from settings.h
inline constexpr uint8_t panelMadctl[N_PANELS] = PANEL_MADCTL_LIST;
inline constexpr bool panelInvert[N_PANELS] = PANEL_INVERT_LIST;
inline constexpr View panelDefaultView[N_PANELS] = PANEL_DEFAULT_VIEW_LIST;

inline constexpr PanelConfig displayPanels[N_PANELS] = {
  { PIN_CS[0], PIN_RST[0], PIN_BL[0], TFT_W, TFT_H, panelMadctl[0], panelInvert[0], 0, 0, panelDefaultView[0], false },
  { PIN_CS[1], PIN_RST[1], PIN_BL[1], TFT_W, TFT_H, panelMadctl[1], panelInvert[1], 0, 0, panelDefaultView[1], true },
  { PIN_CS[2], PIN_RST[2], PIN_BL[2], TFT_W, TFT_H, panelMadctl[2], panelInvert[2], 0, 0, panelDefaultView[2], true },
};

static_assert(sizeof(displayPanels) / sizeof(displayPanels[0]) == N_PANELS,
              "displayPanels count must equal N_PANELS");
