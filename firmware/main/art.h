// Cover art from the PC: `@a begin <id> <w> <h> [444]`, `@a d <base64>` lines,
// `@a end <id> <crc32 hex>` carry a w x h RGB565 big-endian image (RGB444
// packed on the wire with 444). The PC fits the cover to a 224x144 box keeping
// its shape: w 72..224, h 96..144, both even. The media view asks for the art
// of media.art_id; when it is missing the board sends `!art <id>` over USB (at
// most every 5 s) and the PC resends it.
#pragma once
#include <stdint.h>

constexpr int ART_BOX_W = 224, ART_BOX_H = 144;   // the box the media face centres the art in
constexpr int ART_MIN_W = 72, ART_MIN_H = 96;

void artLine(const char *line);   // pcstats hands "@a ..." lines here
// RGB565 big-endian pixels and their size, nullptr = not here (yet)
const uint16_t *artGet(uint32_t id, int *w, int *h);
