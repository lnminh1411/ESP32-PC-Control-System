// Text the board fonts cannot draw (anything non-ASCII: Japanese titles,
// accents, emoji). The board asks the PC to render it (`!txt`), the PC sends
// a 4-bit alpha bitmap (`@x` lines), and the screens blend it in the current
// text colour. Until it arrives each non-ASCII character shows as '?'.
#pragma once
#include <LovyanGFX.hpp>

void textImgLine(const char *line);   // pcstats routes "@x ..." here

// Draw str (the current value of pcText(key)) with c's font, datum and text
// colour, like c.drawString. Returns the width drawn.
int textImgDraw(LGFX_Sprite &c, const char *key, const char *str, int x, int y);
int textImgWidth(LGFX_Sprite &c, const char *key, const char *str);

// Pinned keys hold text the PC never sent (e.g. the weather place from
// settings.h). Their requests carry the text, and the rendered image is kept
// in flash, so it still draws while the PC is off.
void textImgPin(const char *key);
// Load or ask for str's image now, without drawing (call it every frame).
void textImgPrefetch(const char *key, const char *str, const lgfx::IFont *font);
