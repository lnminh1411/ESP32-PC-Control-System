// Dashboard layout engine: dynamic JSON-driven screens bound to PC stats.
//
// Screens are defined as JSON widget trees (/ffat/layout.json or built-in default).
// Widgets bind to pcStat (numbers) or pcText (strings) with delta redraws.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace lgfx {
inline namespace v1 {
class LGFX_Sprite;
}
}
using LGFX_Sprite = lgfx::LGFX_Sprite;

void layoutInit();                       // after gifFsInit(): load /ffat/layout.json, else built-in default
void layoutDraw(int dashIdx, LGFX_Sprite &c, bool full);
     // draw dash page dashIdx (0-based rank of this panel among panels in VIEW_DASH)
     // full = canvas was just cleared (view entered); else redraw widgets in place
uint32_t layoutPeriodMs(int dashIdx);    // shortest widget refresh hint, 0 = data-driven only
void layoutCommand(const char *a1, const char *a2);   // the `layout` console command

// HTTP PUT /layout and upload helper: validate, save atomically to /ffat/layout.json, reload
const char *layoutApply(const char *json, size_t len);
