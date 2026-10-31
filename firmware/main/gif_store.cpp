#include "gif_store.h"

#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings_or_example.h"

Gif gif;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

extern const uint8_t gifsBinStart[] asm("_binary_gifs_bin_start");
extern const uint8_t gifsBinEnd[] asm("_binary_gifs_bin_end");

// Frame tables in PSRAM (76 B a frame); the player reads a few rects per
// frame, nothing it would notice
struct GifTables {
  uint16_t durMs[MAX_FRAMES];
  const uint8_t *z[MAX_FRAMES];
  uint32_t zLen[MAX_FRAMES];
  uint8_t key[MAX_FRAMES];
  uint8_t nRects[MAX_FRAMES];
  GifRect rects[MAX_FRAMES][MAX_DIRTY_RECTS];
};

static tinfl_decompressor *s_inf;   // 11 KB, PSRAM: internal RAM inflated no faster (15-16 ms a frame either way)
static uint8_t *s_raw;              // w*h, PSRAM: a non-key frame's rects before they go into cur

bool gifFsInit() {
  const esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 4,
    .allocation_unit_size = 4096,
    .disk_status_check_enable = false,
    .use_one_fat = false,
  };
  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/ffat", "ffat", &mount_config, &s_wl_handle);
  if (err != ESP_OK) return false;
  if (remove("/ffat/gif.bin") == 0) logMsg("gif: removed the old upload /ffat/gif.bin");
  return true;
}

static bool have(const uint8_t *p, size_t n) { return (size_t)(gifsBinEnd - p) >= n; }

static uint16_t rd16(const uint8_t *&p) {
  uint16_t v;
  memcpy(&v, p, 2);
  p += 2;
  return v;
}

static uint32_t rd32(const uint8_t *&p) {
  uint32_t v;
  memcpy(&v, p, 4);
  p += 4;
  return v;
}

static bool inflateFrame(tinfl_decompressor *d, const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) {
  tinfl_init(d);
  size_t inLen = srcLen, outLen = dstLen;
  tinfl_status st = tinfl_decompress(d, src, &inLen, dst, dst, &outLen,
                                     TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return st == TINFL_STATUS_DONE && outLen == dstLen;
}

// The GIFs after the pack's header, into the tables
static bool readGifs(const uint8_t *p, uint8_t nGif, uint16_t total) {
  uint16_t f = 0;
  for (uint8_t s = 0; s < nGif; s++) {
    if (!have(p, 4)) return false;
    uint16_t nColors = rd16(p), nFrames = rd16(p);
    if (nColors == 0 || nColors > 256 || nFrames == 0 || f + nFrames > total || !have(p, nColors * 2u)) return false;
    gif.pal[s] = (uint16_t *)heap_caps_calloc(256, 2, MALLOC_CAP_INTERNAL);
    if (!gif.pal[s]) return false;
    memcpy(gif.pal[s], p, nColors * 2u);
    p += nColors * 2u;
    gif.segStart[s] = f;
    for (uint16_t i = 0; i < nFrames; i++, f++) {
      if (!have(p, 4)) return false;
      gif.durMs[f] = rd16(p);
      gif.key[f] = *p++ & 1;
      uint8_t nr = *p++;
      if ((i == 0 && !gif.key[f]) || nr > MAX_DIRTY_RECTS || !have(p, nr * sizeof(GifRect) + 4)) return false;
      size_t rawLen = 0;
      for (uint8_t r = 0; r < nr; r++) {
        GifRect &q = gif.rects[f][r];
        memcpy(&q, p, sizeof q);
        p += sizeof q;
        if (q.x + q.w > gif.w || q.y + q.h > gif.h || ((q.x | q.w) & 1)) return false;
        rawLen += (size_t)q.w * q.h;
      }
      if (rawLen > (size_t)gif.w * gif.h) return false;   // overlapping rects would overrun s_raw
      gif.nRects[f] = nr;
      gif.zLen[f] = rd32(p);
      if (!have(p, gif.zLen[f])) return false;
      gif.z[f] = p;
      p += gif.zLen[f];
    }
  }
  gif.segStart[nGif] = f;
  return f == total;
}

bool gifLoad() {
  const uint8_t *p = gifsBinStart;
  if (!have(p, 16) || memcmp(p, "GIFP", 4) || p[4] != 1) return false;
  uint8_t nGif = p[5];
  p += 6;
  gif.w = rd16(p);
  gif.h = rd16(p);
  uint16_t total = rd16(p);
  p += 4;   // inflated size: unused, frames inflate as they play
  if (nGif == 0 || nGif > MAX_GIF_SEGS || gif.w == 0 || gif.w > TFT_W || (gif.w & 1) || gif.h == 0 || gif.h > TFT_H ||
      total == 0 || total > MAX_FRAMES) return false;
  size_t screen = (size_t)gif.w * gif.h;
  GifTables *t = (GifTables *)heap_caps_calloc(1, sizeof(GifTables), MALLOC_CAP_SPIRAM);
  s_raw = (uint8_t *)heap_caps_malloc(screen, MALLOC_CAP_SPIRAM);
  s_inf = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  bool ok = t && s_raw && s_inf;
  if (ok) {
    gif.durMs = t->durMs;
    gif.z = t->z;
    gif.zLen = t->zLen;
    gif.key = t->key;
    gif.nRects = t->nRects;
    gif.rects = t->rects;
    ok = readGifs(p, nGif, total);
  }
  if (!ok) {
    for (uint16_t *&pal : gif.pal) { free(pal); pal = nullptr; }
    free(s_inf);
    free(s_raw);
    free(t);
    s_inf = nullptr;
    s_raw = nullptr;
    return false;
  }
  gif.bytes = sizeof(GifTables) + screen;
  gif.packBytes = gifsBinEnd - gifsBinStart;
  gif.nSeg = nGif;
  gif.n = total;   // last: the player starts when this is set
  return true;
}

bool gifApplyFrame(uint8_t *cur, uint16_t f) {
  if (gif.key[f]) return inflateFrame(s_inf, gif.z[f], gif.zLen[f], cur, (size_t)gif.w * gif.h);
  size_t rawLen = 0;
  for (uint8_t i = 0; i < gif.nRects[f]; i++) rawLen += (size_t)gif.rects[f][i].w * gif.rects[f][i].h;
  if (!inflateFrame(s_inf, gif.z[f], gif.zLen[f], s_raw, rawLen)) return false;
  const uint8_t *s = s_raw;
  for (uint8_t i = 0; i < gif.nRects[f]; i++) {
    const GifRect &r = gif.rects[f][i];
    for (int y = r.y; y < r.y + r.h; y++) {
      memcpy(cur + (size_t)y * gif.w + r.x, s, r.w);
      s += r.w;
    }
  }
  return true;
}

bool gifBuildFrame(uint8_t *cur, uint16_t f) {
  uint16_t k = f;
  while (!gif.key[k]) k--;   // gifLoad checked: a segment starts with a key frame
  for (uint16_t i = k; i <= f; i++)
    if (!gifApplyFrame(cur, i)) return false;
  return true;
}

uint8_t gifSegOf(uint16_t f) {
  uint8_t s = 0;
  while (s + 1 < gif.nSeg && f >= gif.segStart[s + 1]) s++;
  return s;
}

uint16_t gifPrevFrame(uint16_t f) {
  uint8_t s = gifSegOf(f);
  return f == gif.segStart[s] ? gif.segStart[s + 1] - 1 : f - 1;
}

static uint8_t segDue(uint32_t nowMs) {
  return gif.nSeg > 1 ? (uint8_t)(nowMs / (GIF_SWITCH_S * 1000UL) % gif.nSeg) : 0;
}

uint16_t gifNextFrame(uint16_t idx, uint32_t nowMs) {
  if (idx >= gif.n) return 0;
  uint8_t s = gifSegOf(idx);
  if (idx + 1 < gif.segStart[s + 1]) return idx + 1;
  return gif.segStart[segDue(nowMs)];   // a loop ended: this segment again, or the one whose turn it is
}

uint16_t gifStartFrame(int slot, int nSlots, uint32_t nowMs) {
  if (!gif.n) return 0;
  uint8_t s = segDue(nowMs);
  uint16_t len = gif.segStart[s + 1] - gif.segStart[s];
  return gif.segStart[s] + (uint16_t)(slot * len / nSlots);
}
