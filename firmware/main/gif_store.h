// GIF frames, built into the firmware: main/gifs.bin (from scripts/gif_pack.py,
// sources in firmware/gifs/) stays compressed in flash; each frame is inflated
// as it plays (display.cpp's gifdec task, core 1). Each GIF is a segment with
// its own palette of RGB444 colours: segments loop on their own and take
// turns, GIF_SWITCH_S each. A frame is 8-bit palette indices: a key frame
// (each GIF's first) holds the whole screen, any other only the pixels of its
// dirty rects against the frame before it in the segment's loop.
//
// Pack format (little-endian, nothing aligned):
//   "GIFP", u8 version (1), u8 nGif, u16 w, u16 h, u16 frames in all GIFs,
//   u32 bytes of indices once inflated (unused since frames inflate as they play)
//   per GIF: u16 nColors, u16 nFrames, nColors x u16 colour (0x0RGB)
//   per frame: u16 duration_ms, u8 flags (bit 0 key frame), u8 nRects,
//     nRects x u16 x, y, w, h (x and w even), u32 zLen, zLen bytes of zlib:
//     key frame w*h indices, else the indices of its rects, rect by rect,
//     row-major
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define MAX_DIRTY_RECTS 8
#define MAX_GIF_SEGS 8

struct GifRect {
  uint16_t x, y, w, h;
};

// The per-frame tables are one PSRAM block (gifLoad allocates it at boot,
// before anything reads them); n stays 0 without it.
struct Gif {
  uint16_t w, h, n;               // pushed size (w even), frame count
  uint16_t *durMs;                // [MAX_FRAMES]
  const uint8_t **z;              // [MAX_FRAMES] zlib data in the pack (flash)
  uint32_t *zLen;                 // [MAX_FRAMES]
  uint8_t *key;                   // [MAX_FRAMES] the whole screen, not its rects
  uint8_t *nRects;                // [MAX_FRAMES] up to 8 dirty rects vs previous frame
  GifRect (*rects)[MAX_DIRTY_RECTS];   // [MAX_FRAMES]
  uint32_t bytes;                 // PSRAM: tables and the inflate buffer
  uint32_t packBytes;             // gifs.bin in flash
  uint8_t nSeg;                   // segments (GIFs)
  uint16_t segStart[MAX_GIF_SEGS + 1];   // segment s: frames segStart[s] .. segStart[s + 1] - 1
  uint16_t *pal[MAX_GIF_SEGS];    // [256] 0x0RGB per segment, internal RAM: read for every pixel pushed
};

extern Gif gif;

bool gifFsInit();                               // mount FAT; false = not available
bool gifLoad();                                 // index the built-in pack; false = bad / no memory

// cur: a panel's w*h indices. Apply needs frame gifPrevFrame(f) in cur, and
// inflates f's rects over it; Build makes frame f from any state (inflates
// every frame from the segment's key frame). One task only: they share one
// inflate buffer. false = the frame did not inflate.
bool gifApplyFrame(uint8_t *cur, uint16_t f);
bool gifBuildFrame(uint8_t *cur, uint16_t f);

uint8_t gifSegOf(uint16_t f);
uint16_t gifPrevFrame(uint16_t f);              // the frame before f in its segment's loop
// The frame after idx: its segment loops until that loop ends past the
// segment's turn (GIF_SWITCH_S, one clock for every panel), then the next
// segment starts. slot/nSlots spread panels over a segment (start frame).
uint16_t gifNextFrame(uint16_t idx, uint32_t nowMs);
uint16_t gifStartFrame(int slot, int nSlots, uint32_t nowMs);
