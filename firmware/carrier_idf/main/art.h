// Cover art from the PC: `@a begin <id> 144 144`, `@a d <base64>` lines,
// `@a end <id> <crc32 hex>` carry a 144x144 RGB565 big-endian image. The
// media view asks for the art of media.art_id; when it is missing the board
// sends `!art <id>` over HID (at most every 5 s) and the PC resends it.
#pragma once
#include <stdint.h>

constexpr int ART_SIZE = 144;

void artLine(const char *line);                  // pcstats hands "@a ..." lines here
const uint16_t *artGet(uint32_t id);             // RGB565 big-endian pixels, nullptr = not here (yet)
