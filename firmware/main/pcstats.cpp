#include "pcstats.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "art.h"
#include "textimg.h"
#include "board_time.h"
#include "pc_going.h"
#include "sys.h"
#include "settings_or_example.h"

struct StatEntry {
  char key[24];
  double val;   // double: whole numbers exact up to 2^53 (uptime, token counters)
};

struct TextEntry {
  char key[24];
  char val[121];   // UTF-8, the PC cuts values at 120 bytes
};

// Both tables live in PSRAM (14 KB internal RAM saved), allocated with the
// first line; until then count = textCount = 0 and nothing reads them.
static StatEntry *table;
static int count;

static TextEntry *textTable;
static int textCount;

static uint32_t lastSampleMs;
static uint32_t sampleSeq;

void pcStatsLine(const char *line) {
  if (!line || line[0] != '@') return;
  if (!table) {
    table = (StatEntry *)heap_caps_calloc(PCSTATS_MAX, sizeof(StatEntry), MALLOC_CAP_SPIRAM);
    textTable = (TextEntry *)heap_caps_calloc(PCSTATS_TEXT_MAX, sizeof(TextEntry), MALLOC_CAP_SPIRAM);
    if (!table || !textTable) {
      heap_caps_free(table);
      heap_caps_free(textTable);
      table = nullptr;
      textTable = nullptr;
      return;
    }
  }
  if (line[1] == 'a' && line[2] == ' ') { artLine(line); return; }
  if (line[1] == 'x' && line[2] == ' ') { textImgLine(line); return; }
  if (line[1] == 's' && (line[2] == ' ' || line[2] == '\0')) {
    const char *p = line + 2;
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;
      const char *start = p;
      while (*p && *p != ' ' && *p != '=') p++;
      if (*p != '=') {
        while (*p && *p != ' ') p++;
        continue;
      }
      size_t klen = (size_t)(p - start);
      char kbuf[24];
      if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
      memcpy(kbuf, start, klen);
      kbuf[klen] = '\0';

      p++; // skip '='
      double v = NAN;
      if (*p && *p != ' ') {
        char *end;
        v = strtod(p, &end);
        p = end;
      }

      int idx = -1;
      for (int i = 0; i < count; i++) {
        if (!strcmp(table[i].key, kbuf)) {
          idx = i;
          break;
        }
      }
      if (idx >= 0) {
        table[idx].val = v;
      } else if (count < PCSTATS_MAX) {
        snprintf(table[count].key, sizeof(table[count].key), "%s", kbuf);
        table[count].key[sizeof(table[count].key) - 1] = '\0';
        table[count].val = v;
        count++;
      }
    }
  } else if (line[1] == 't' && (line[2] == ' ' || line[2] == '\0')) {
    const char *p = line + 2;
    while (*p == ' ') p++;
    if (!*p) return;
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t klen = (size_t)(p - start);
    char kbuf[24];
    if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
    memcpy(kbuf, start, klen);
    kbuf[klen] = '\0';

    if (*p == ' ') p++; // skip delimiter space between key and text
    const char *tstart = p;
    char tbuf[sizeof(TextEntry::val)];   // the whole 120 bytes: a cut title no longer matches the PC's CRC
    size_t tlen = strlen(tstart);
    if (tlen >= sizeof(tbuf)) tlen = sizeof(tbuf) - 1;
    memcpy(tbuf, tstart, tlen);
    tbuf[tlen] = '\0';
    while (tlen > 0 && (tbuf[tlen - 1] == '\r' || tbuf[tlen - 1] == '\n')) {
      tbuf[--tlen] = '\0';
    }

    int idx = -1;
    for (int i = 0; i < textCount; i++) {
      if (!strcmp(textTable[i].key, kbuf)) {
        idx = i;
        break;
      }
    }
    if (idx >= 0) {
      snprintf(textTable[idx].val, sizeof(textTable[idx].val), "%s", tbuf);
      textTable[idx].val[sizeof(textTable[idx].val) - 1] = '\0';
    } else if (textCount < PCSTATS_TEXT_MAX) {
      snprintf(textTable[textCount].key, sizeof(textTable[textCount].key), "%s", kbuf);
      textTable[textCount].key[sizeof(textTable[textCount].key) - 1] = '\0';
      snprintf(textTable[textCount].val, sizeof(textTable[textCount].val), "%s", tbuf);
      textTable[textCount].val[sizeof(textTable[textCount].val) - 1] = '\0';
      textCount++;
    }
  } else if (line[1] == 'e') {
    lastSampleMs = nowMs();
    sampleSeq++;
    pcGoingOnSample();
    boardTimeOnSample();
  }
}

double pcStat(const char *key) {
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= PCSTATS_STALE_MS) return NAN;
  if (!key) return NAN;
  for (int i = 0; i < count; i++) {
    if (!strcmp(table[i].key, key)) return table[i].val;
  }
  return NAN;
}

bool pcStatsFresh() {
  return lastSampleMs && (uint32_t)(nowMs() - lastSampleMs) < PCSTATS_FRESH_MS;
}

int pcStatsCount() {
  return count;
}

const char *pcStatKey(int i) {
  return (i >= 0 && i < count) ? table[i].key : nullptr;
}

double pcStatAt(int i) {
  return (i >= 0 && i < count) ? table[i].val : NAN;
}

const char *pcText(const char *key) {
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= PCSTATS_STALE_MS) return "";
  if (!key) return "";
  for (int i = 0; i < textCount; i++) {
    if (!strcmp(textTable[i].key, key)) return textTable[i].val;
  }
  return "";
}

int pcTextCount() {
  return textCount;
}

const char *pcTextKey(int i) {
  return (i >= 0 && i < textCount) ? textTable[i].key : nullptr;
}

const char *pcTextAt(int i) {
  return (i >= 0 && i < textCount) ? textTable[i].val : "";
}

uint32_t pcStatsSeq() {
  return sampleSeq;
}
