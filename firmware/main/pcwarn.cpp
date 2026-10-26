#include "pcwarn.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "pcstats.h"
#include "sys.h"

#define AVG_MS_DEFAULT 2000
#define AVG_MS_MIN 500
#define AVG_MS_MAX 5000
#define SAMPLES 12   // the PC sends a sample every 500 ms: room for a 5 s window

struct Samples {
  uint32_t t[SAMPLES];
  float v[SAMPLES];
  uint8_t next, count;
};

// Thresholds: the defaults below, overridden per metric from NVS ("pcwarn"
// namespace, "<key>.h" / "<key>.c" in tenths, -1 = off) by `pcwarn set`
struct Metric {
  const char *key, *label;
  bool temp;
  float defHigh, defCrit;  // NAN: no such level
  float high, crit;
};

static Metric metrics[] = {
  {"ram_load", "RAM", false, 80.0f, 90.0f, NAN, NAN},
  {"cpu_load", "CPU", false, 90.0f, NAN, NAN, NAN},
  {"gpu1_load", "GPU1", false, 90.0f, NAN, NAN, NAN},
  {"gpu2_load", "GPU2", false, 90.0f, NAN, NAN, NAN},
  {"gpu1_vram_pct", "VRAM", false, 90.0f, NAN, NAN, NAN},   // RX 6600 only: used / total
  {"cpu_temp", "CPU", true, NAN, 80.0f, NAN, NAN},
  {"gpu1_temp", "GPU1", true, NAN, 80.0f, NAN, NAN},
  {"gpu2_temp", "GPU2", true, NAN, 75.0f, NAN, NAN},
  {"ram_temp", "RAM", true, NAN, 75.0f, NAN, NAN},
  {"vrm_temp", "VRM", true, NAN, 75.0f, NAN, NAN},
};
#define N_METRICS ((int)(sizeof metrics / sizeof metrics[0]))

static Samples *samples;       // N_METRICS of them, PSRAM
static uint32_t avgMs = AVG_MS_DEFAULT;
static bool configLoaded;
static uint32_t lastSeq;
static uint32_t demoUntilMs;   // `pcwarn demo`: fixed values instead of the PC's
static bool demoOne;

static void loadConfig() {
  configLoaded = true;
  for (Metric &m : metrics) { m.high = m.defHigh; m.crit = m.defCrit; }
  avgMs = AVG_MS_DEFAULT;
  nvs_handle_t h;
  if (nvs_open("pcwarn", NVS_READONLY, &h) != ESP_OK) return;
  int32_t v;
  if (nvs_get_i32(h, "avg_ms", &v) == ESP_OK && v >= AVG_MS_MIN && v <= AVG_MS_MAX) avgMs = (uint32_t)v;
  for (Metric &m : metrics) {
    char k[16];
    snprintf(k, sizeof k, "%s.h", m.key);
    if (nvs_get_i32(h, k, &v) == ESP_OK) m.high = v < 0 ? NAN : v / 10.0f;
    snprintf(k, sizeof k, "%s.c", m.key);
    if (nvs_get_i32(h, k, &v) == ESP_OK) m.crit = v < 0 ? NAN : v / 10.0f;
  }
  nvs_close(h);
}

static float average(int i, uint32_t now) {
  if (!samples) return NAN;
  const Samples &s = samples[i];
  float sum = 0;
  int n = 0;
  for (int k = 0; k < s.count; k++) {
    if (now - s.t[k] > avgMs) continue;
    sum += s.v[k];
    n++;
  }
  return n ? sum / n : NAN;
}

void pcWarnPoll() {
  if (!configLoaded) loadConfig();
  uint32_t seq = pcStatsSeq();
  if (seq == lastSeq) return;
  lastSeq = seq;
  if (!samples) samples = (Samples *)heap_caps_calloc(N_METRICS, sizeof(Samples), MALLOC_CAP_SPIRAM);
  if (!samples) return;
  uint32_t now = nowMs();
  for (int i = 0; i < N_METRICS; i++) {
    const Metric &m = metrics[i];
    float v = pcStat(m.key);
    if (!strcmp(m.key, "gpu1_vram_pct")) {   // not a PC stat: derived here
      float used = pcStat("gpu1_vram"), total = pcStat("gpu1_vramt");
      v = total > 0 ? used / total * 100.0f : NAN;
    }
    if (isnan(v)) continue;
    Samples &s = samples[i];
    s.t[s.next] = now;
    s.v[s.next] = v;
    s.next = (uint8_t)((s.next + 1) % SAMPLES);
    if (s.count < SAMPLES) s.count++;
  }
}

// demo values: every warning at once (RAM critical), or RAM high alone
static float demoValue(int i) {
  if (demoOne) return !strcmp(metrics[i].key, "ram_load") ? 85.0f : 10.0f;
  static const float vals[] = {93, 96, 97, 95, 94, 86, 84, 79, 77, 78};   // table order
  return vals[i];
}

static bool demoOn(uint32_t now) { return demoUntilMs && (int32_t)(demoUntilMs - now) > 0; }

static PcWarnLevel levelOf(const Metric &m, float avg, float *limit) {
  if (isnan(avg)) return PCWARN_NONE;
  if (!isnan(m.crit) && avg > m.crit) { *limit = m.crit; return PCWARN_CRITICAL; }
  if (!isnan(m.high) && avg > m.high) { *limit = m.high; return PCWARN_HIGH; }
  return PCWARN_NONE;
}

uint32_t pcWarnAvgMs() { return avgMs; }

PcWarnLevel pcWarnLevel(const char *key) {
  uint32_t now = nowMs();
  bool demo = demoOn(now);
  if (!demo && !pcStatsFresh()) return PCWARN_NONE;
  float limit;
  for (int i = 0; i < N_METRICS; i++)
    if (!strcmp(metrics[i].key, key)) return levelOf(metrics[i], demo ? demoValue(i) : average(i, now), &limit);
  return PCWARN_NONE;
}

int pcWarnList(PcWarn *out) {
  uint32_t now = nowMs();
  bool demo = demoOn(now);
  if (!demo && !pcStatsFresh()) return 0;   // PC off or service gone: nothing to judge
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {    // critical first, then high
    for (int i = 0; i < N_METRICS && n < PCWARN_MAX; i++) {
      const Metric &m = metrics[i];
      float avg = demo ? demoValue(i) : average(i, now), limit = NAN;
      PcWarnLevel level = levelOf(m, avg, &limit);
      if (level == PCWARN_NONE || (pass == 0) != (level == PCWARN_CRITICAL)) continue;
      out[n++] = {m.label, avg, limit, m.temp, level};
    }
  }
  return n;
}

static void fmtLimit(char *out, size_t cap, float v) {
  if (isnan(v)) snprintf(out, cap, "off");
  else snprintf(out, cap, "%.1f", v);
}

static void show() {
  uint32_t now = nowMs();
  logMsg("pcwarn: %lu ms average%s", (unsigned long)avgMs, demoOn(now) ? ", demo on" : "");
  for (int i = 0; i < N_METRICS; i++) {
    const Metric &m = metrics[i];
    char hi[24], cr[24];
    fmtLimit(hi, sizeof hi, m.high);
    fmtLimit(cr, sizeof cr, m.crit);
    logMsg("  %-14s now %6.1f%s | high %s, crit %s", m.key, average(i, now), m.temp ? " C" : " %", hi, cr);
  }
  PcWarn w[PCWARN_MAX];
  logMsg("pcwarn: %d active | pcwarn set <metric> <high|crit> <value|off|default> | avg <ms> | reset | demo [one] | off",
         pcWarnList(w));
}

static bool saveI32(const char *key, int32_t v, bool erase) {
  nvs_handle_t h;
  if (nvs_open("pcwarn", NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = erase ? nvs_erase_key(h, key) : nvs_set_i32(h, key, v);
  if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

void pcWarnCommand(const char *args) {
  if (!configLoaded) loadConfig();
  char buf[96];
  snprintf(buf, sizeof buf, "%s", args ? args : "");
  char *save = nullptr;
  char *a1 = strtok_r(buf, " \t", &save), *a2 = strtok_r(nullptr, " \t", &save);
  char *a3 = strtok_r(nullptr, " \t", &save), *a4 = strtok_r(nullptr, " \t", &save);
  if (!a1) { show(); return; }

  if (!strcmp(a1, "demo")) {
    demoOne = a2 && !strcmp(a2, "one");
    demoUntilMs = nowMs() + 60000;
    logMsg("pcwarn: demo (%s) for 60 s", demoOne ? "RAM high only" : "every warning");
  } else if (!strcmp(a1, "off")) {
    demoUntilMs = 0;
    logMsg("pcwarn: demo off");
  } else if (!strcmp(a1, "avg")) {
    long ms = a2 ? strtol(a2, nullptr, 10) : 0;
    if (ms < AVG_MS_MIN || ms > AVG_MS_MAX) { logMsg("pcwarn: avg takes %d-%d ms", AVG_MS_MIN, AVG_MS_MAX); return; }
    avgMs = (uint32_t)ms;
    logMsg("pcwarn: %lu ms average%s", (unsigned long)avgMs, saveI32("avg_ms", (int32_t)ms, false) ? ", saved" : " (not saved)");
  } else if (!strcmp(a1, "reset")) {
    nvs_handle_t h;
    if (nvs_open("pcwarn", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    loadConfig();
    logMsg("pcwarn: defaults back");
  } else if (!strcmp(a1, "set") && a2 && a3 && a4) {
    Metric *m = nullptr;
    for (Metric &x : metrics) if (!strcmp(x.key, a2)) m = &x;
    bool high = !strcmp(a3, "high"), crit = !strcmp(a3, "crit");
    if (!m || (!high && !crit)) { logMsg("pcwarn: usage pcwarn set <metric> <high|crit> <value|off|default> (metrics: see pcwarn)"); return; }
    char k[16];
    snprintf(k, sizeof k, "%s.%c", m->key, high ? 'h' : 'c');
    float &slot = high ? m->high : m->crit;
    bool ok;
    if (!strcmp(a4, "default")) { slot = high ? m->defHigh : m->defCrit; ok = saveI32(k, 0, true); }
    else if (!strcmp(a4, "off")) { slot = NAN; ok = saveI32(k, -1, false); }
    else {
      float v = strtof(a4, nullptr);
      if (!(v > 0 && v <= 150)) { logMsg("pcwarn: value 0-150, off or default"); return; }
      slot = v;
      ok = saveI32(k, (int32_t)lroundf(v * 10), false);
    }
    char lim[24];
    fmtLimit(lim, sizeof lim, slot);
    logMsg("pcwarn: %s %s = %s%s", m->key, a3, lim, ok ? ", saved" : " (not saved)");
  } else {
    show();
  }
}
