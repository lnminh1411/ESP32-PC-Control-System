#include "art.h"

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sys.h"
#include "usb.h"

constexpr size_t ART_BYTES = (size_t)ART_SIZE * ART_SIZE * 2;

// Two PSRAM buffers: the one on screen stays valid while the next arrives
static uint8_t *shown, *incoming;
static uint32_t shownId, incomingId;
static size_t incomingLen, incomingSize;   // incomingSize: RGB444 (packed) or RGB565 bytes expected
static uint32_t lastRequestMs;
static uint32_t lastRequestId;

static bool ensureBuffers() {
  if (shown) return true;
  shown = (uint8_t *)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
  incoming = (uint8_t *)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
  if (shown && incoming) return true;
  free(shown);
  free(incoming);
  shown = incoming = nullptr;
  return false;
}

void artLine(const char *line) {
  // "@a begin <id> <w> <h>" / "@a d <b64>" / "@a end <id> <crc>"
  if (strncmp(line, "@a ", 3) || !ensureBuffers()) return;
  const char *rest = line + 3;
  if (!strncmp(rest, "begin ", 6)) {
    unsigned long id = 0, w = 0, h = 0, fmt = 565;
    if (sscanf(rest + 6, "%lu %lu %lu %lu", &id, &w, &h, &fmt) < 3 || w != ART_SIZE || h != ART_SIZE) {
      incomingId = 0;
      return;
    }
    incomingId = (uint32_t)id;
    incomingLen = 0;
    incomingSize = fmt == 444 ? ART_BYTES * 3 / 4 : ART_BYTES;
  } else if (!strncmp(rest, "d ", 2)) {
    if (!incomingId) return;
    size_t out = 0;
    const char *b64 = rest + 2;
    if (mbedtls_base64_decode(incoming + incomingLen, incomingSize - incomingLen, &out,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
      logMsg("art: bad chunk, dropping %lu", (unsigned long)incomingId);
      incomingId = 0;
      return;
    }
    incomingLen += out;
  } else if (!strncmp(rest, "end ", 4)) {
    unsigned long id = 0, crc = 0;
    if (sscanf(rest + 4, "%lu %lx", &id, &crc) != 2 || id != incomingId) return;
    uint32_t got = esp_rom_crc32_le(0, incoming, incomingLen);
    if (incomingLen != incomingSize || got != (uint32_t)crc) {
      logMsg("art: %lu incomplete (%u bytes, crc %08lx vs %08lx)", id, (unsigned)incomingLen,
             (unsigned long)got, crc);
      incomingId = 0;
      return;
    }
    if (incomingSize != ART_BYTES) {
      // RGB444 pairs (3 bytes) -> RGB565 big-endian, in place from the end:
      // each output pair (4 bytes) only overwrites input already consumed
      for (int j = ART_SIZE * ART_SIZE / 2 - 1; j >= 0; j--) {
        const uint8_t *q = incoming + 3 * j;
        int r0 = q[0] >> 4, g0 = q[0] & 15, b0 = q[1] >> 4, r1 = q[1] & 15, g1 = q[2] >> 4, b1 = q[2] & 15;
        uint16_t c0 = (uint16_t)((r0 << 12) | (r0 >> 3 << 11) | (g0 << 7) | (g0 >> 2 << 5) | (b0 << 1) | (b0 >> 3));
        uint16_t c1 = (uint16_t)((r1 << 12) | (r1 >> 3 << 11) | (g1 << 7) | (g1 >> 2 << 5) | (b1 << 1) | (b1 >> 3));
        uint8_t *o = incoming + 4 * j;
        o[0] = c0 >> 8; o[1] = c0 & 0xFF; o[2] = c1 >> 8; o[3] = c1 & 0xFF;
      }
    }
    uint8_t *t = shown;   // swap: the finished image goes on screen
    shown = incoming;
    incoming = t;
    shownId = incomingId;
    incomingId = 0;
  }
}

const uint16_t *artGet(uint32_t id) {
  if (!id) return nullptr;
  if (id == shownId && shown) return (const uint16_t *)shown;
  // missing: ask the PC, unless a transfer of it is running or we just asked
  uint32_t now = nowMs();
  if (id != incomingId && (id != lastRequestId || now - lastRequestMs >= 5000)) {
    char req[24];
    snprintf(req, sizeof req, "!art %lu\n", (unsigned long)id);
    usbPcSendLine(req);
    lastRequestId = id;
    lastRequestMs = now;
  }
  return nullptr;
}
