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

// Frame tables in PSRAM: 6 KB of internal RAM back; the player reads a few
// rects per frame, nothing it would notice
struct GifTables {
  uint16_t durMs[MAX_FRAMES];
  uint8_t *data[MAX_FRAMES];
  uint8_t key[MAX_FRAMES];
  uint8_t nRects[MAX_FRAMES];
  GifRect rects[MAX_FRAMES][MAX_DIRTY_RECTS];
};

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

// The GIFs after the pack's header, into the tables and out[bytes]
static bool readGifs(const uint8_t *p, uint8_t nGif, uint16_t total, tinfl_decompressor *d, uint8_t *out, uint32_t bytes) {
  uint16_t f = 0;
  uint32_t used = 0;
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
      size_t rawLen = gif.key[f] ? (size_t)gif.w * gif.h : 0;
      for (uint8_t r = 0; r < nr; r++) {
        GifRect &q = gif.rects[f][r];
        memcpy(&q, p, sizeof q);
        p += sizeof q;
        if (q.x + q.w > gif.w || q.y + q.h > gif.h || ((q.x | q.w) & 1)) return false;
        if (!gif.key[f]) rawLen += (size_t)q.w * q.h;
      }
      gif.nRects[f] = nr;
      uint32_t zLen = rd32(p);
      if (!have(p, zLen) || rawLen > bytes - used || !inflateFrame(d, p, zLen, out + used, rawLen)) return false;
      gif.data[f] = out + used;
      used += rawLen;
      p += zLen;
    }
  }
  gif.segStart[nGif] = f;
  return f == total && used == bytes;
}

bool gifLoad() {
  const uint8_t *p = gifsBinStart;
  if (!have(p, 16) || memcmp(p, "GIFP", 4) || p[4] != 1) return false;
  uint8_t nGif = p[5];
  p += 6;
  gif.w = rd16(p);
  gif.h = rd16(p);
  uint16_t total = rd16(p);
  uint32_t bytes = rd32(p);
  if (nGif == 0 || nGif > MAX_GIF_SEGS || gif.w == 0 || gif.w > TFT_W || (gif.w & 1) || gif.h == 0 || gif.h > TFT_H ||
      total == 0 || total > MAX_FRAMES) return false;
  GifTables *t = (GifTables *)heap_caps_calloc(1, sizeof(GifTables), MALLOC_CAP_SPIRAM);
  uint8_t *out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  tinfl_decompressor *d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_INTERNAL);   // 11 KB: too much for this stack
  bool ok = t && out && d;
  if (ok) {
    gif.durMs = t->durMs;
    gif.data = t->data;
    gif.key = t->key;
    gif.nRects = t->nRects;
    gif.rects = t->rects;
    ok = readGifs(p, nGif, total, d, out, bytes);
  }
  free(d);
  if (!ok) {
    for (uint16_t *&pal : gif.pal) { free(pal); pal = nullptr; }
    free(out);
    free(t);
    return false;
  }
  gif.bytes = bytes;
  gif.nSeg = nGif;
  gif.n = total;   // last: the player starts when this is set
  return true;
}

void gifApplyFrame(uint8_t *cur, uint16_t f) {
  const uint8_t *s = gif.data[f];
  bool key = gif.key[f];
  for (uint8_t i = 0; i < gif.nRects[f]; i++) {
    const GifRect &r = gif.rects[f][i];
    for (int y = r.y; y < r.y + r.h; y++) {
      size_t off = (size_t)y * gif.w + r.x;
      if (key) {
        memcpy(cur + off, s + off, r.w);
      } else {
        memcpy(cur + off, s, r.w);
        s += r.w;
      }
    }
  }
}

void gifBuildFrame(uint8_t *cur, uint16_t f) {
  uint16_t k = f;
  while (!gif.key[k]) k--;   // gifLoad checked: a segment starts with a key frame
  memcpy(cur, gif.data[k], (size_t)gif.w * gif.h);
  for (uint16_t i = k + 1; i <= f; i++) gifApplyFrame(cur, i);
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
