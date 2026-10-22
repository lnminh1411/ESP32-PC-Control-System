// Views: what each panel shows. The GIF runs at its own frame timing; the
// two dashboards redraw every 250/500 ms, but only changed glyphs hit the bus.
//   gif    -- the stored GIF, centred (panels showing it start at offset frames)
//   status -- PC state, boot-guard countdown, uptime, last action, door/RST
//   stats  -- bus load, per-panel fps and KB/s, memory, chip temperature
#pragma once
#include <stdint.h>

#include "display.h"

const char *viewName(View v);
bool viewParse(const char *name, View &v);   // false = not a view name
void viewEnter(int panel, View v);
void viewsService();                         // call from loop(): draw + flush what is due
void viewsShowGifFrame(const uint16_t *frame);   // draw on every GIF panel now (upload preview)
void viewsNoteAction(const char *what);      // "last action" on the status view
float busLoadPct();                          // whole SPI bus, last 1 s window
