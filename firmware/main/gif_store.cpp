#include "gif_store.h"

#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings_or_example.h"

Gif gif;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static const uint8_t *s_packEnd;   // end of the mapped gifs partition
static uint32_t s_seed;            // turn order, new each boot

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

static bool have(const uint8_t *p, size_t n) { return (size_t)(s_packEnd - p) >= n; }

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

// One LZ4 block (no frame header, no stored size) into exactly dstLen bytes.
// Every length and offset is checked against both buffers: false = corrupt.
static bool lz4Block(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) {
  const uint8_t *ip = src, *iEnd = src + srcLen;
  uint8_t *op = dst, *oEnd = dst + dstLen;
  auto more = [&](size_t &n) {   // a 15 nibble: 255-bytes until one below 255
    uint8_t b;
    do {
      if (ip == iEnd) return false;
      b = *ip++;
      n += b;
    } while (b == 255);
    return true;
  };
  while (ip < iEnd) {
    uint8_t tok = *ip++;
    size_t n = tok >> 4;
    if (n == 15 && !more(n)) return false;
    if ((size_t)(iEnd - ip) < n || (size_t)(oEnd - op) < n) return false;
    memcpy(op, ip, n);
    op += n;
    ip += n;
    if (ip == iEnd) break;   // the last sequence is literals only
    if (iEnd - ip < 2) return false;
    size_t off = ip[0] | ip[1] << 8;
    ip += 2;
    n = tok & 15;
    if (n == 15 && !more(n)) return false;
    n += 4;
    if (off == 0 || off > (size_t)(op - dst) || (size_t)(oEnd - op) < n) return false;
    // an overlapping match repeats its last off bytes: copy whole periods,
    // the copied run doubling each pass
    for (size_t d = off; n;) {
      size_t c = n < d ? n : d;
      memcpy(op, op - d, c);
      op += c;
      n -= c;
      d += c;
    }
  }
  return op == oEnd;
}

// The GIFs after the pack's header, into the tables; p ends past the last frame
static bool readGifs(const uint8_t *&p, uint8_t nGif, uint16_t total) {
  uint16_t f = 0;
  for (uint8_t s = 0; s < nGif; s++) {
    if (!have(p, 4)) return false;
    uint16_t nColors = rd16(p), nFrames = rd16(p);
    if (nColors == 0 || nColors > GIF_SKIP || nFrames == 0 || f + nFrames > total || !have(p, nColors * 2u)) return false;
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
        // display.cpp packs 8 pixels at a time: a rect is a multiple of 8
        if (!q.w || !q.h || q.x + q.w > gif.w || q.y + q.h > gif.h || ((q.x | q.w) & 1) || ((q.w * q.h) & 7)) return false;
        rawLen += (size_t)q.w * q.h;
      }
      if (rawLen > (size_t)gif.w * gif.h) return false;   // overlapping rects would overrun raw
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
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "gifs");
  const void *map;
  esp_partition_mmap_handle_t mapH;
  if (!part || esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &mapH) != ESP_OK) return false;
  const uint8_t *start = (const uint8_t *)map, *p = start;
  s_packEnd = start + part->size;
  if (!have(p, 16) || memcmp(p, "GIFP", 4) || p[4] != 3) { esp_partition_munmap(mapH); return false; }
  uint8_t nGif = p[5];
  p += 6;
  gif.w = rd16(p);
  gif.h = rd16(p);
  uint16_t total = rd16(p);
  p += 4;   // decoded size: unused, frames decode as they play
  if (nGif == 0 || nGif > MAX_GIF_SEGS || gif.w == 0 || gif.w > TFT_W || gif.h == 0 || gif.h > TFT_H ||
      ((gif.w | gif.h) & (GIF_BLOCK - 1)) || total == 0 || total > MAX_FRAMES) { esp_partition_munmap(mapH); return false; }
  GifTables *t = (GifTables *)heap_caps_calloc(1, sizeof(GifTables), MALLOC_CAP_SPIRAM);
  bool ok = t;
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
    free(t);
    esp_partition_munmap(mapH);
    return false;
  }
  s_seed = esp_random();
  gif.bytes = sizeof(GifTables);
  gif.packBytes = p - start;
  gif.nSeg = nGif;
  gif.n = total;   // last: the player starts when this is set
  return true;
}

size_t gifRawBytes() {
  return (size_t)gif.w * gif.h + (size_t)(gif.w / GIF_BLOCK) * (gif.h / GIF_BLOCK) * 2;
}

// Every block's shift stays on the frame: false = corrupt
static bool shiftsFit(const int8_t *mv) {
  for (int y = 0; y < gif.h; y += GIF_BLOCK) {
    for (int x = 0; x < gif.w; x += GIF_BLOCK, mv += 2) {
      int sy = y + mv[0], sx = x + mv[1];
      if (sy < 0 || sy + GIF_BLOCK > gif.h || sx < 0 || sx + GIF_BLOCK > gif.w) return false;
    }
  }
  return true;
}

// raw's GIF_SKIP pixels in shifted blocks from cur, the frame before, rect by
// rect, a block's run of a row at a time. They read pixels the next step
// overwrites, and most blocks have no shift: done first, and only those.
static void fillShifted(const uint8_t *cur, uint8_t *s, const int8_t *mv, uint16_t f) {
  int cols = gif.w / GIF_BLOCK;
  for (uint8_t i = 0; i < gif.nRects[f]; i++) {
    const GifRect &r = gif.rects[f][i];
    for (int y = r.y; y < r.y + r.h; y++) {
      const int8_t *row = mv + (size_t)(y / GIF_BLOCK) * cols * 2;
      for (int x = r.x, end = r.x + r.w; x < end;) {
        int n = GIF_BLOCK - x % GIF_BLOCK;
        if (n > end - x) n = end - x;
        const int8_t *m = row + (x / GIF_BLOCK) * 2;
        if (m[0] | m[1]) {
          const uint8_t *q = cur + (ptrdiff_t)(y + m[0]) * gif.w + x + m[1];
          for (int k = 0; k < n; k++)
            if (s[k] == GIF_SKIP) s[k] = q[k];
        }
        s += n;
        x += n;
      }
    }
  }
}

bool gifApplyFrame(uint8_t *cur, uint8_t *raw, uint16_t f) {
  if (gif.key[f]) return lz4Block(gif.z[f], gif.zLen[f], cur, (size_t)gif.w * gif.h);
  size_t rawLen = 0;
  for (uint8_t i = 0; i < gif.nRects[f]; i++) rawLen += (size_t)gif.rects[f][i].w * gif.rects[f][i].h;
  if (!lz4Block(gif.z[f], gif.zLen[f], raw, rawLen + gifRawBytes() - (size_t)gif.w * gif.h)) return false;
  const int8_t *mv = (const int8_t *)raw + rawLen;
  if (!shiftsFit(mv)) return false;
  fillShifted(cur, raw, mv, f);
  // what is left of GIF_SKIP has no shift: cur has it, raw needs it for flush.
  // One pass both ways, PSRAM bandwidth being what limits a frame
  uint8_t *s = raw;
  for (uint8_t i = 0; i < gif.nRects[f]; i++) {
    const GifRect &r = gif.rects[f][i];
    for (int y = 0; y < r.h; y++, s += r.w) {
      uint8_t *d = cur + (size_t)(r.y + y) * gif.w + r.x;
      for (int k = 0; k < r.w; k++) {
        if (s[k] == GIF_SKIP) s[k] = d[k];
        else d[k] = s[k];
      }
    }
  }
  return true;
}

bool gifBuildFrame(uint8_t *cur, uint8_t *raw, uint16_t f) {
  uint16_t k = f;
  while (!gif.key[k]) k--;   // gifLoad checked: a segment starts with a key frame
  for (uint16_t i = k; i <= f; i++)
    if (!gifApplyFrame(cur, raw, i)) return false;
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

// Every GIF once in a shuffled round, then a new shuffle
static void roundOrder(uint32_t round, uint8_t *order) {
  uint32_t x = (round + 1) * 2654435761u ^ s_seed;
  for (uint8_t i = 0; i < gif.nSeg; i++) order[i] = i;
  for (uint8_t i = gif.nSeg - 1; i > 0; i--) {
    x = x * 1664525u + 1013904223u;
    uint8_t j = (x >> 16) % (i + 1), t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
}

// Whose turn it is. From the clock alone, so every panel agrees. A round never
// starts with the GIF the last one ended on (swapping 0 and 1 leaves the last
// place alone, which is what the next round compares with).
static uint8_t segDue(uint32_t nowMs) {
  if (gif.nSeg < 2) return 0;
  uint32_t turn = nowMs / (GIF_SWITCH_S * 1000UL);
  if (gif.nSeg == 2) return turn % 2;
  uint32_t round = turn / gif.nSeg;
  uint8_t order[MAX_GIF_SEGS], last[MAX_GIF_SEGS];
  roundOrder(round, order);
  if (round) {
    roundOrder(round - 1, last);
    if (order[0] == last[gif.nSeg - 1]) { order[0] = order[1]; order[1] = last[gif.nSeg - 1]; }
  }
  return order[turn % gif.nSeg];
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
