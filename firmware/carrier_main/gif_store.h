// GIF frames: held in PSRAM, saved to /gif.bin in the FAT partition,
// uploaded over the UART by scripts/gif_stream.py.
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

struct Gif {
  uint16_t w, h, n;
  uint16_t durMs[MAX_FRAMES];
  uint16_t *frames[MAX_FRAMES];   // RGB565, big-endian (as received)
};

extern Gif gif;

bool gifFsInit();                               // mount FAT; false = not available
bool gifLoad();                                 // from flash; false = none / bad
// After the "GIF5" magic has been read: header, frames, save. onFrame is
// called with each frame as it lands, so it can be shown during the upload.
bool gifReceive(void (*onFrame)(const uint16_t *frame));
