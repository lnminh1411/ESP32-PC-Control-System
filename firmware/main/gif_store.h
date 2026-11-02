// GIF frames, in their own flash partition: main/gifs.bin (from scripts/gif_pack.py,
// sources in firmware/gifs/) stays compressed there, memory-mapped; each frame is decoded
// as it plays (display.cpp's gifdec task, core 1). Each GIF is a segment with
// its own palette of RGB444 colours: segments loop on their own and take
// turns in random cycles, GIF_SWITCH_S each, every GIF once a cycle. A frame is 8-bit palette indices: a key frame
// (each GIF's first) holds the whole screen, any other only the pixels of its
// dirty rects against the frame before it in the segment's loop, and most of those
// come from that frame: each 8x8 block has a shift into it, and a GIF_SKIP pixel
// takes the pixel its block's shift lands on. The packer lets such a pixel be up to a
// GIF's "lossy" colour levels off (gifs.json), never on the backdrop colour.
//
// Pack format (little-endian, nothing aligned):
//   "GIFP", u8 version (3; 2 had no shifts, 1 had zlib frames), u8 nGif, u16 w, u16 h,
//   u16 frames in all GIFs, u32 bytes of indices once decoded (unused)
//   per GIF: u16 nColors (up to 255), u16 nFrames, nColors x u16 colour (0x0RGB)
//   per frame: u16 duration_ms, u8 flags (bit 0 key frame), u8 nRects,
//     nRects x u16 x, y, w, h (x and w even, w*h a multiple of 8, as is the
//     GIF's w*h: display.cpp packs 8 pixels at a time), u32 zLen, zLen bytes of one
//     LZ4 block (no size stored): key frame w*h indices, else the indices of
//     its rects, rect by rect, row-major, then (dy, dx) as i8 for every block,
//     row-major, each landing wholly on the frame
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define MAX_DIRTY_RECTS 8
#define MAX_GIF_SEGS 12
#define GIF_BLOCK 8                     // px, and the GIF's w and h are multiples of it
#define GIF_SKIP 255                    // index: take the shifted pixel from the frame before

struct GifRect {
  uint16_t x, y, w, h;
};

// The per-frame tables are one PSRAM block (gifLoad allocates it at boot,
// before anything reads them); n stays 0 without it.
struct Gif {
  uint16_t w, h, n;               // pushed size (w even), frame count
  uint16_t *durMs;                // [MAX_FRAMES]
  const uint8_t **z;              // [MAX_FRAMES] LZ4 block in the pack (mapped flash)
  uint32_t *zLen;                 // [MAX_FRAMES]
  uint8_t *key;                   // [MAX_FRAMES] the whole screen, not its rects
  uint8_t *nRects;                // [MAX_FRAMES] up to 8 dirty rects vs previous frame
  GifRect (*rects)[MAX_DIRTY_RECTS];   // [MAX_FRAMES]
  uint32_t bytes;                 // PSRAM: the tables
  uint32_t packBytes;             // gifs.bin in the gifs partition
  uint8_t nSeg;                   // segments (GIFs)
  uint16_t segStart[MAX_GIF_SEGS + 1];   // segment s: frames segStart[s] .. segStart[s + 1] - 1
  uint16_t *pal[MAX_GIF_SEGS];    // [256] 0x0RGB per segment, internal RAM: read for every pixel pushed
};

extern Gif gif;

bool gifFsInit();                               // mount FAT; false = not available
bool gifLoad();                                 // index the built-in pack; false = bad / no memory

// cur: a panel's w*h indices, raw: gifRawBytes() for the decoded rects. Apply
// needs all of frame gifPrevFrame(f) in cur: it leaves f's rects in raw, back to
// back, and all of f in cur (a key frame decodes whole into cur, raw unused).
// Build makes frame f in cur from any state (decodes every frame from the
// segment's key frame). Any task, each with its own buffers. false = the frame
// did not decode.
size_t gifRawBytes();
bool gifApplyFrame(uint8_t *cur, uint8_t *raw, uint16_t f);
bool gifBuildFrame(uint8_t *cur, uint8_t *raw, uint16_t f);

uint8_t gifSegOf(uint16_t f);
uint16_t gifPrevFrame(uint16_t f);              // the frame before f in its segment's loop
// The frame after idx: its segment loops until that loop ends past the
// segment's turn (GIF_SWITCH_S, shared by every panel), then the next segment
// starts. A played list keeps every GIF to once a cycle, and a GIF stays away
// for at least half the GIFs across cycles. Main task. slot/nSlots spread panels over a segment (start frame).
uint16_t gifNextFrame(uint16_t idx, uint32_t nowMs);
uint16_t gifStartFrame(int slot, int nSlots, uint32_t nowMs);
