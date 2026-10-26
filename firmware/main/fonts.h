// UI fonts: Comic Shanns Mono Nerd Font as anti-aliased LovyanGFX VLW fonts,
// generated into font_comic*.cpp by scripts/make_vlw.py (ASCII + degree/micro).
// Each is parsed once on first use; glyph bitmaps stay in flash.
#pragma once
#include <LovyanGFX.hpp>

const lgfx::IFont *fontSmall();   // 12 px regular: labels, metric rows
const lgfx::IFont *fontBody();    // 16 px regular: values
const lgfx::IFont *fontHead();    // 20 px bold: panel titles
const lgfx::IFont *fontBig();     // 28 px bold: the big status values
const lgfx::IFont *fontDigits();  // 56 px bold, digits . - : % and degree only: weather temp, tok/s
const lgfx::IFont *fontClock();   // 128 px bold, digits only: the stacked clock
const lgfx::IFont *fontIcons();   // 44 px Nerd Font weather icons (U+E3xx)
const lgfx::IFont *fontIconsSm(); // 18 px weather + media/status icons
