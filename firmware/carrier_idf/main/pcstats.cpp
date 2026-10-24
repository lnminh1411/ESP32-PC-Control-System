#include "pcstats.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

struct StatEntry {
  char key[24];
  double val;   // double: whole numbers exact up to 2^53 (uptime, token counters)
};

struct TextEntry {
  char key[24];
  char val[81];
};

static StatEntry table[PCSTATS_MAX];
static int count;

static TextEntry textTable[PCSTATS_TEXT_MAX];
static int textCount;

static uint32_t lastSampleMs;

void pcStatsLine(const char *line) {
  if (!line || line[0] != '@') return;
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
    char tbuf[81];
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
  }
}

double pcStat(const char *key) {
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= 5000) return NAN;
  if (!key) return NAN;
  for (int i = 0; i < count; i++) {
    if (!strcmp(table[i].key, key)) return table[i].val;
  }
  return NAN;
}

bool pcStatsFresh() {
  return lastSampleMs && (uint32_t)(nowMs() - lastSampleMs) < 3000;
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
  if (!lastSampleMs || (uint32_t)(nowMs() - lastSampleMs) >= 5000) return "";
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
