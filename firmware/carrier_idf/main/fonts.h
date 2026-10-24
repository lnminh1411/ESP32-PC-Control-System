// UI fonts: Comic Shanns Mono Nerd Font as anti-aliased LovyanGFX VLW fonts,
// generated into font_comic*.cpp by scripts/make_vlw.py (ASCII + degree/micro).
// Each is parsed once on first use; glyph bitmaps stay in flash.
#pragma once
#include <LovyanGFX.hpp>

const lgfx::IFont *fontSmall();   // 12 px regular: labels, metric rows
const lgfx::IFont *fontBody();    // 16 px regular: values
const lgfx::IFont *fontHead();    // 20 px bold: panel titles
const lgfx::IFont *fontBig();     // 28 px bold: the big status values
