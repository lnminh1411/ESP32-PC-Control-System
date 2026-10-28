#include "pcstats.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

// The main loop writes the tables; the Discord bot task reads them too. Keys
// never change once counted, so lookups go unlocked; values (a double is two
// words, a text a string) are written and read under this spinlock, a few
// dozen bytes at a time.
static portMUX_TYPE statMux = portMUX_INITIALIZER_UNLOCKED;

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
        taskENTER_CRITICAL(&statMux);
        table[idx].val = v;
        taskEXIT_CRITICAL(&statMux);
      } else if (count < PCSTATS_MAX) {
        memcpy(table[count].key, kbuf, klen + 1);
        table[count].val = v;
        taskENTER_CRITICAL(&statMux);   // key and value in place before readers see the entry
        count++;
        taskEXIT_CRITICAL(&statMux);
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
      taskENTER_CRITICAL(&statMux);
      memcpy(textTable[idx].val, tbuf, tlen + 1);
      taskEXIT_CRITICAL(&statMux);
    } else if (textCount < PCSTATS_TEXT_MAX) {
      memcpy(textTable[textCount].key, kbuf, klen + 1);
      memcpy(textTable[textCount].val, tbuf, tlen + 1);
      taskENTER_CRITICAL(&statMux);   // key and text in place before readers see the entry
      textCount++;
      taskEXIT_CRITICAL(&statMux);
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
    if (!strcmp(table[i].key, key)) {
      taskENTER_CRITICAL(&statMux);
      double v = table[i].val;
      taskEXIT_CRITICAL(&statMux);
      return v;
    }
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
  if (i < 0 || i >= count) return NAN;
  taskENTER_CRITICAL(&statMux);
  double v = table[i].val;
  taskEXIT_CRITICAL(&statMux);
  return v;
}

const char *pcText(const char *key) {
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= PCSTATS_STALE_MS) return "";
  if (!key) return "";
  for (int i = 0; i < textCount; i++) {
    if (!strcmp(textTable[i].key, key)) return textTable[i].val;
  }
  return "";
}

bool pcTextCopy(const char *key, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= PCSTATS_STALE_MS) return false;
  if (!key) return false;
  for (int i = 0; i < textCount; i++) {
    if (!strcmp(textTable[i].key, key)) {
      taskENTER_CRITICAL(&statMux);
      size_t n = strnlen(textTable[i].val, sizeof(textTable[i].val) - 1);
      if (n >= cap) n = cap - 1;
      memcpy(out, textTable[i].val, n);
      out[n] = 0;
      taskEXIT_CRITICAL(&statMux);
      return true;
    }
  }
  return false;
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
