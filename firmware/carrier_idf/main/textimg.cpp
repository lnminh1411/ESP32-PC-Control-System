#include "textimg.h"

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fonts.h"
#include "log.h"
#include "sys.h"
#include "usb.h"

// One rendered string at one font size
struct TextImg {
  uint32_t id;           // crc32 of the UTF-8 text
  uint8_t px, bold;
  uint16_t w, h, ascent;
  uint8_t *alpha;        // PSRAM, (w + 1) / 2 bytes per row, high nibble = left pixel
  uint32_t usedMs;
};

constexpr int CACHE_N = 8;
constexpr int MAX_W = 1600, MAX_H = 64;
static TextImg cache[CACHE_N];

// The transfer in progress
static TextImg incoming;
static size_t incomingLen, incomingCap;

// Requests already sent: don't ask again for 5 s
struct Asked { uint32_t id; uint8_t px, bold; uint32_t ms; };
static Asked asked[4];
static uint32_t lastAskMs;

static bool isAscii(const char *s) {
  for (; *s; s++) if ((uint8_t)*s >= 0x80) return false;
  return true;
}

static bool fontSize(const lgfx::IFont *f, uint8_t &px, uint8_t &bold) {
  if (f == fontSmall()) { px = 12; bold = 0; return true; }
  if (f == fontBody())  { px = 16; bold = 0; return true; }
  if (f == fontHead())  { px = 20; bold = 1; return true; }
  if (f == fontBig())   { px = 28; bold = 1; return true; }
  return false;
}

static TextImg *find(uint32_t id, uint8_t px, uint8_t bold) {
  for (TextImg &t : cache)
    if (t.alpha && t.id == id && t.px == px && t.bold == bold) return &t;
  return nullptr;
}

static void request(const char *key, uint32_t id, uint8_t px, uint8_t bold) {
  uint32_t now = nowMs();
  if (incoming.alpha && incoming.id == id) return;   // on its way
  int slot = 0;
  for (int i = 0; i < 4; i++) {
    if (asked[i].id == id && asked[i].px == px && asked[i].bold == bold && now - asked[i].ms < 5000) return;
    if (asked[i].ms < asked[slot].ms) slot = i;
  }
  if (now - lastAskMs < 200) return;   // one at a time; the next frame asks again
  char req[64];
  snprintf(req, sizeof req, "!txt %lu %u %u %s\n", (unsigned long)id, px, bold, key);
  usbHidSendLine(req);
  asked[slot] = {id, px, bold, now};
  lastAskMs = now;
}

void textImgLine(const char *line) {
  // "@x begin <id> <px> <bold> <w> <h> <ascent>" / "@x d <b64>" / "@x end <id> <crc>"
  if (strncmp(line, "@x ", 3)) return;
  const char *rest = line + 3;
  if (!strncmp(rest, "begin ", 6)) {
    unsigned long id = 0;
    unsigned px = 0, bold = 0, w = 0, h = 0, asc = 0;
    free(incoming.alpha);
    incoming = {};
    if (sscanf(rest + 6, "%lu %u %u %u %u %u", &id, &px, &bold, &w, &h, &asc) != 6 ||
        !w || w > MAX_W || !h || h > MAX_H || asc > h) return;
    incomingCap = (size_t)(w + 1) / 2 * h;
    incoming.alpha = (uint8_t *)heap_caps_malloc(incomingCap, MALLOC_CAP_SPIRAM);
    if (!incoming.alpha) return;
    incoming.id = (uint32_t)id;
    incoming.px = (uint8_t)px;
    incoming.bold = (uint8_t)bold;
    incoming.w = (uint16_t)w;
    incoming.h = (uint16_t)h;
    incoming.ascent = (uint16_t)asc;
    incomingLen = 0;
  } else if (!strncmp(rest, "d ", 2)) {
    if (!incoming.alpha) return;
    size_t out = 0;
    const char *b64 = rest + 2;
    if (mbedtls_base64_decode(incoming.alpha + incomingLen, incomingCap - incomingLen, &out,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
      logMsg("textimg: bad chunk, dropping %lu", (unsigned long)incoming.id);
      free(incoming.alpha);
      incoming = {};
      return;
    }
    incomingLen += out;
  } else if (!strncmp(rest, "end ", 4)) {
    unsigned long id = 0, crc = 0;
    if (!incoming.alpha || sscanf(rest + 4, "%lu %lx", &id, &crc) != 2 || id != incoming.id) return;
    if (incomingLen != incomingCap || esp_rom_crc32_le(0, incoming.alpha, incomingLen) != (uint32_t)crc) {
      logMsg("textimg: %lu incomplete (%u of %u bytes)", id, (unsigned)incomingLen, (unsigned)incomingCap);
      free(incoming.alpha);
      incoming = {};
      return;
    }
    // replace the same string at the same size, else the least recently used
    TextImg *slot = find(incoming.id, incoming.px, incoming.bold);
    if (!slot) {
      slot = &cache[0];
      for (TextImg &t : cache) {
        if (!t.alpha) { slot = &t; break; }
        if (t.usedMs < slot->usedMs) slot = &t;
      }
    }
    free(slot->alpha);
    *slot = incoming;
    slot->usedMs = nowMs();
    incoming = {};
  }
}

// The cached image for str in c's current font, asking the PC when missing
static TextImg *lookup(LGFX_Sprite &c, const char *key, const char *str) {
  if (!str || !str[0] || isAscii(str)) return nullptr;
  uint8_t px, bold;
  if (!fontSize(c.getFont(), px, bold)) return nullptr;
  uint32_t id = esp_rom_crc32_le(0, (const uint8_t *)str, strlen(str));
  TextImg *t = find(id, px, bold);
  if (t) t->usedMs = nowMs();
  else request(key, id, px, bold);
  return t;
}

// '?' for each non-ASCII character (UTF-8 continuation bytes dropped)
static void asciiFallback(const char *str, char *out, size_t cap) {
  size_t n = 0;
  for (const uint8_t *s = (const uint8_t *)str; *s && n + 1 < cap; s++) {
    if (*s < 0x80) out[n++] = (char)*s;
    else if ((*s & 0xC0) != 0x80) out[n++] = '?';
  }
  out[n] = 0;
}

int textImgWidth(LGFX_Sprite &c, const char *key, const char *str) {
  TextImg *t = lookup(c, key, str);
  if (t) return t->w;
  if (!str || isAscii(str)) return c.textWidth(str ? str : "");
  char fb[128];
  asciiFallback(str, fb, sizeof fb);
  return c.textWidth(fb);
}

int textImgDraw(LGFX_Sprite &c, const char *key, const char *str, int x, int y) {
  TextImg *t = lookup(c, key, str);
  if (!t) {
    if (!str || isAscii(str)) return c.drawString(str ? str : "", x, y);
    char fb[128];
    asciiFallback(str, fb, sizeof fb);
    return c.drawString(fb, x, y);
  }

  // place it where drawString would put this font's glyphs: same datum, same baseline
  lgfx::FontMetrics m;
  c.getFont()->getDefaultMetric(&m);
  uint8_t datum = c.getTextDatum();
  int left = x - ((datum & 3) == 1 ? t->w / 2 : (datum & 3) == 2 ? t->w : 0);
  int cellTop = (datum & 16) ? y - m.baseline : (datum & 8) ? y - m.height : (datum & 4) ? y - m.height / 2 : y;
  int top = cellTop + m.baseline - t->ascent;

  int32_t cx, cy, cw, ch;
  c.getClipRect(&cx, &cy, &cw, &ch);
  uint32_t fg = c.getTextStyle().fore_rgb888;
  int cr = (fg >> 16) & 0xFF, cg = (fg >> 8) & 0xFF, cb = fg & 0xFF;
  int stride = (t->w + 1) / 2;
  for (int j = 0; j < t->h; j++) {
    int py = top + j;
    if (py < cy || py >= cy + ch) continue;
    const uint8_t *row = t->alpha + j * stride;
    for (int i = 0; i < t->w; i++) {
      int a = (i & 1) ? (row[i >> 1] & 0x0F) : (row[i >> 1] >> 4);
      int px = left + i;
      if (!a || px < cx || px >= cx + cw) continue;
      uint16_t bg = c.readPixel(px, py);
      int br = ((bg >> 11) & 0x1F) << 3, bgG = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
      int r = br + (cr - br) * a / 15, g = bgG + (cg - bgG) * a / 15, b = bb + (cb - bb) * a / 15;
      c.drawPixel(px, py, (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
    }
  }
  return t->w;
}
