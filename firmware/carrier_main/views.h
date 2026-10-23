// Views: what each panel shows.
//   gif    -- pre-packed RGB444 frames with precomputed dirty rects, DMA bypass
//   status -- PC state, boot-guard countdown, uptime, last action, door/RST
//   stats  -- bus load, per-panel fps and KB/s, memory, chip temperature
// Core 1 draws into canvas / selects GIF frame; core 0 flush task diffs/sends.
#pragma once
#include <stdint.h>

#include "display.h"

const char *viewName(View v);
bool viewParse(const char *name, View &v);   // false = not a view name
void viewEnter(int panel, View v);
void viewsService();                         // call from loop(): serve due panels to flush task
void viewsShowGifFrame(int idx);             // preview frame idx on every GIF panel now
void viewsNoteAction(const char *what);      // "last action" on the status view
float busLoadPct();                          // whole SPI bus wire load, last 1 s window
