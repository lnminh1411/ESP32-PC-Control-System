// GIF frames: pre-packed RGB444 with precomputed dirty rects held in PSRAM,
// saved as RGB565-BE to /gif.bin in FAT, uploaded over UART by scripts/gif_stream.py.
//
// Upload protocol (host -> device unless noted; little-endian):
//   device: "READY\n" after boot, then 1.5 s for an upload to start
//   "GIF5" u16 w, u16 h, u16 n, n x u16 duration_ms
//   device: "OK\n" or "ERR <reason>\n"
//   per frame: w*h*2 bytes RGB565 big-endian, u32 byte-sum
//   device: "K<i>\n" (stored) or "E<i>\n" (sum mismatch -> host resends)
//   device: "SAVED\n" (or "ERR save"), then "PLAY\n"
// A new upload is accepted any time: an upper-case 'G' at the start of a
// UART line (commands are all lower-case).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define MAX_DIRTY_RECTS 8

struct GifRect {
  uint16_t x, y, w, h;
};

struct Gif {
  uint16_t w, h, n;
  uint16_t pw;                    // packed width = w rounded UP to even (pad pixel black)
  uint16_t durMs[MAX_FRAMES];
  uint8_t *packed[MAX_FRAMES];    // pw*h*3/2 bytes each, PSRAM, exact push444 byte layout
  uint8_t nRects[MAX_FRAMES];     // up to 8 dirty rects vs previous frame
  GifRect rects[MAX_FRAMES][MAX_DIRTY_RECTS];
};

extern Gif gif;

bool gifFsInit();                               // mount FAT; false = not available
bool gifLoad();                                 // from flash; false = none / bad
// After the "GIF5" magic has been read: header, frames, save. onFrame is
// called with each frame index as it lands, so it can be shown during upload.
bool gifReceive(void (*onFrame)(int idx));
