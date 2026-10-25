// Desk screens (Screen 1: PC stats/GIF, Screen 2: Media/Weather, Screen 3: AI/Clock).
// Ported from .mock/screens_mockup.html to pure ESP-IDF 5.5 C++ with LovyanGFX.
#include "screens.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <LovyanGFX.hpp>

#include "art.h"
#include "board_time.h"
#include "config.h"
#include "display.h"
#include "fonts.h"
#include "pc_going.h"
#include "pc_state.h"
#include "pcstats.h"
#include "sys.h"
#include "textimg.h"
#include "weather.h"

// 16-bit RGB565 palette matching the mockup (survives 4 bits per channel wire format).
constexpr uint16_t C_BG = 0x0000;            // #000000
constexpr uint16_t C_CARD = 0x18C5;          // #1a1a2a
constexpr uint16_t C_TRACK = 0x3188;         // #333344
constexpr uint16_t C_TEXT = 0xEF7D;          // #eeeeee
constexpr uint16_t C_DIM = 0x8CD5;           // #8899aa
constexpr uint16_t C_FAINT = 0x532E;         // #556677
constexpr uint16_t C_CPU = 0x455F;           // #44aaff
constexpr uint16_t C_RAM = 0xBC5F;           // #bb88ff
constexpr uint16_t C_AMD = 0xFAA8;           // #ff5544
constexpr uint16_t C_NV = 0x8EE6;            // #88dd33
constexpr uint16_t C_DISK = 0xFD46;          // #ffaa33
constexpr uint16_t C_FAN = 0x46F9;           // #44ddcc
constexpr uint16_t C_GOOD = 0x46EC;          // #44dd66
constexpr uint16_t C_WARN = 0xFE64;          // #ffcc22
constexpr uint16_t C_BAD = 0xFA26;           // #ff4433
constexpr uint16_t C_PURPLE = 0xAABB;        // #aa55dd
constexpr uint16_t C_CLAUDE = 0xEBAA;        // #ee7755
constexpr uint16_t C_AGY = 0x64DF;           // #6699ff

// Weather screen palette
constexpr uint16_t C_WX_BG = 0x1108;         // #112244
constexpr uint16_t C_WX_CLOUD = 0xDF7F;      // #ddeeff
constexpr uint16_t C_WX_RAIN = 0x8E7F;       // #88ccff
constexpr uint16_t C_WX_TEXT_DIM = 0x74D7;   // #7799bb
constexpr uint16_t C_WX_TEXT_MID = 0xCEFD;   // #ccddee
constexpr uint16_t C_WX_BOX_BG = 0x218A;     // #223355
constexpr uint16_t C_WX_DIV = 0x322C;        // #334466

// Local AI screen palette
constexpr uint16_t C_AI_BG = 0x1043;         // #110a1a
constexpr uint16_t C_AI_CELL = 0x20C6;       // #221833
constexpr uint16_t C_AI_PULSE_DIM = 0x510E;  // #552277

// Clock palette
constexpr uint16_t C_CLOCK_HOURS = 0x9E7D;   // #99ccee

// Threshold severity color picker: good if < warn, warn if < bad, else bad.
static inline uint16_t sev(double v, double w, double b) {
  if (isnan(v)) return C_DIM;
  if (v >= b) return C_BAD;
  if (v >= w) return C_WARN;
  return C_GOOD;
}

// Rounded card background container.
static inline void drawCard(LGFX_Sprite &c, int x, int y, int w, int h) {
  c.fillRoundRect(x, y, w, h, 6, C_CARD);
}

// Rounded horizontal progress bar with track and thumb.
static void drawBar(LGFX_Sprite &c, int x, int y, int w, int h, float frac, uint16_t hex) {
  int r = h >> 1;
  c.fillRoundRect(x, y, w, h, r, C_TRACK);
  if (isnan(frac) || frac <= 0.0f) return;
  if (frac > 1.0f) frac = 1.0f;
  int fw = (int)roundf((float)w * frac);
  if (fw > 1) {
    int fillW = fw < h ? h : fw;
    if (fillW > w) fillW = w;
    c.fillRoundRect(x, y, fillW, h, r, hex);
  }
}

// Small rounded status badge/chip.
static void drawChip(LGFX_Sprite &c, int x, int y, const char *label, uint16_t bgHex, uint16_t fgHex, bool rightAlign = true) {
  c.setFont(fontSmall());
  int w = c.textWidth(label) + 12;
  int x0 = rightAlign ? (x - w) : x;
  c.fillRoundRect(x0, y, w, 18, 9, bgHex);
  c.setTextColor(fgHex);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(label, x0 + 6, y + 1);
}

// Format byte rate into KB/s or MB/s.
static void formatRate(double b, char *buf, size_t len) {
  if (isnan(b)) {
    snprintf(buf, len, "--");
  } else if (b >= 1048576.0) {
    snprintf(buf, len, "%.1f MB/s", b / 1048576.0);
  } else {
    snprintf(buf, len, "%.1f KB/s", b / 1024.0);
  }
}

// GPU load averaged over ~2 s: at idle clocks the cards report bursty
// utilization (a wallpaper frame keeps a 210 MHz core busy), so the raw
// value swings 0-50% between samples.
static double smoothedStat(int slot, const char *key) {
  static double val[2];
  static uint32_t lastMs[2];
  double v = pcStat(key);
  if (isnan(v)) return v;
  uint32_t now = nowMs();
  if (isnan(val[slot]) || !lastMs[slot] || now - lastMs[slot] > 10000) {
    val[slot] = v;
  } else {
    val[slot] += (v - val[slot]) * (1.0 - exp(-(double)(now - lastMs[slot]) / 2000.0));
  }
  lastMs[slot] = now;
  return val[slot];
}

// Format uptime seconds into "1d 20:44:16" or "20:44:16".
static void formatUptime(double uptimeSec, char *buf, size_t len) {
  if (isnan(uptimeSec) || uptimeSec < 0.0) {
    snprintf(buf, len, "--");
    return;
  }
  uint32_t s = (uint32_t)uptimeSec;
  uint32_t d = s / 86400;
  uint32_t h = (s / 3600) % 24;
  uint32_t m = (s / 60) % 60;
  uint32_t sec = s % 60;
  if (d > 0) {
    snprintf(buf, len, "%lud %02lu:%02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m,
             (unsigned long)sec);
  } else {
    snprintf(buf, len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  }
}

// 56-sample ring buffer for net_down sparkline, sampled on pcStatsSeq() changes.
static float netDownHist[56] = {0};
static int netHistCount = 0;
static int netHistHead = 0;
static uint32_t lastNetSeq = 0;

static void updateNetHistory() {
  uint32_t seq = pcStatsSeq();
  if (seq != lastNetSeq) {
    lastNetSeq = seq;
    double down = pcStat("net_down");
    float v = (isnan(down) || down < 0.0) ? 0.0f : (float)down;
    netDownHist[netHistHead] = v;
    netHistHead = (netHistHead + 1) % 56;
    if (netHistCount < 56) netHistCount++;
  }
}

static void drawSparkline(LGFX_Sprite &c) {
  updateNetHistory();
  float samples[56];
  if (netHistCount < 56) {
    int empty = 56 - netHistCount;
    for (int i = 0; i < empty; i++) samples[i] = 0.0f;
    for (int i = 0; i < netHistCount; i++) samples[empty + i] = netDownHist[i];
  } else {
    for (int i = 0; i < 56; i++) {
      samples[i] = netDownHist[(netHistHead + i) % 56];
    }
  }

  float maxVal = 0.0f;
  for (int i = 0; i < 56; i++) {
    if (samples[i] > maxVal) maxVal = samples[i];
  }

  if (maxVal < 1.0f) {
    c.drawLine(12, 308, 232, 308, C_CPU);
    return;
  }

  for (int i = 1; i < 56; i++) {
    int x0 = 12 + (i - 1) * 4;
    int y0 = 308 - (int)roundf((samples[i - 1] / maxVal) * 18.0f);
    int x1 = 12 + i * 4;
    int y1 = 308 - (int)roundf((samples[i] / maxVal) * 18.0f);
    c.drawLine(x0, y0, x1, y1, C_CPU);
  }
}

// Back-and-forth marquee: 30 px/s, 1.5 s pause at both ends, clip to 224 px window.
static char lastTitle[128] = "";
static uint32_t marqueeStartMs = 0;

static void drawMarquee(LGFX_Sprite &c, const char *key, const char *s, int y, const lgfx::IFont *f, uint16_t hex) {
  c.setFont(f);
  int w = textImgWidth(c, key, s);
  const int room = 224;
  if (w <= room) {
    c.setTextColor(hex);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    textImgDraw(c, key, s, 120, y);
    return;
  }

  // Restart marquee cycle when title text changes
  if (strcmp(s, lastTitle) != 0) {
    strncpy(lastTitle, s, sizeof(lastTitle) - 1);
    lastTitle[sizeof(lastTitle) - 1] = '\0';
    marqueeStartMs = nowMs();
  }

  float travel = (float)(w - room);
  const float speed = 30.0f;
  const float pause = 1.5f;
  float leg = travel / speed;
  float cyc = 2.0f * (leg + pause);
  if (cyc <= 0.0f) cyc = 1.0f;

  float elapsedSec = (float)(nowMs() - marqueeStartMs) / 1000.0f;
  float t = fmodf(elapsedSec, cyc);
  if (t < 0.0f) t += cyc;

  float off = 0.0f;
  if (t < pause) {
    off = 0.0f;
  } else if (t < pause + leg) {
    off = (t - pause) * speed;
  } else if (t < 2.0f * pause + leg) {
    off = travel;
  } else {
    off = travel - (t - 2.0f * pause - leg) * speed;
  }

  c.setClipRect(8, y, room, c.fontHeight());
  c.setTextColor(hex);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  textImgDraw(c, key, s, 8 - (int)roundf(off), y);
  c.clearClipRect();
}

static int measureTextWidth(const char *key, const char *s, const lgfx::IFont *f) {
  if (!s || !s[0]) return 0;
  for (int i = 0; i < N_PANELS; i++) {
    if (panels[i].canvas) {
      panels[i].canvas->setFont(f);
      return textImgWidth(*panels[i].canvas, key, s);
    }
  }
  return (int)strlen(s) * 12;
}

static bool mediaMarqueeMoving() {
  if (pcStat("media.ok") != 1.0) return false;
  const char *title = pcText("media.title");
  if (!title || !title[0]) return false;
  // the scheduler asks on every pass: measure once per title, except while
  // a non-ASCII title's rendered width may still change (its image arrives)
  static char measured[128];
  static bool wide, final;
  if (!final || strcmp(title, measured)) {
    snprintf(measured, sizeof measured, "%s", title);
    wide = measureTextWidth("media.title", title, fontHead()) > 224;
    final = true;
    for (const char *p = title; *p; p++) if ((uint8_t)*p >= 0x80) final = false;
  }
  return wide;
}

// Map WxIcon enum to UTF-8 Nerd Font weather glyph.
static const char *getWxIconUtf8(WxIcon icon, bool isDay) {
  switch (icon) {
    case WX_CLEAR:   return isDay ? "\uE30D" : "\uE32B";
    case WX_PARTLY:  return isDay ? "\uE302" : "\uE37E";
    case WX_CLOUDY:  return "\uE312";
    case WX_FOG:     return "\uE313";
    case WX_DRIZZLE: return "\uE31B";
    case WX_RAIN:    return isDay ? "\uE318" : "\uE325";
    case WX_SHOWERS: return "\uE319";
    case WX_SNOW:    return "\uE31A";
    case WX_STORM:   return "\uE31D";
    default:         return isDay ? "\uE30D" : "\uE32B";
  }
}

// Media player source brand icon and color.
static const char *getMediaSourceIcon(const char *src, uint16_t &iconCol) {
  if (strstr(src, "Spotify") || strstr(src, "spotify")) {
    iconCol = C_GOOD;
    return "\uF1BC";
  } else if (strstr(src, "Chrome") || strstr(src, "chrome")) {
    iconCol = C_WARN;
    return "\uE743";
  } else if (strstr(src, "Firefox") || strstr(src, "firefox")) {
    iconCol = C_CLAUDE;
    return "\uE745";
  } else if (strstr(src, "Edge") || strstr(src, "edge")) {
    iconCol = C_CPU;
    return "\uF282";
  } else {
    iconCol = C_DIM;
    return "\uF001";
  }
}

// Small directional network arrow (down/up).
static inline void drawNetArrow(LGFX_Sprite &c, int x, int y, bool down, uint16_t hex) {
  if (down) {
    c.fillTriangle(x, y, x + 8, y, x + 4, y + 6, hex);
  } else {
    c.fillTriangle(x, y + 6, x + 8, y + 6, x + 4, y, hex);
  }
}

// Warning overlay box for restarting / updating on GIF faces.
static void drawWarnBox(LGFX_Sprite &c, const char *title, const char *sub) {
  c.fillRect(0, OVERLAY_Y0, TFT_W, OVERLAY_Y1 - OVERLAY_Y0, C_BG);

  int y = 248;
  c.fillRoundRect(12, y, 216, 56, 8, C_WARN);
  c.fillRoundRect(15, y + 3, 210, 50, 6, C_BG);

  c.setFont(fontIconsSm());
  c.setTextColor(C_WARN);
  c.setTextDatum(lgfx::textdatum_t::middle_center);
  c.drawString("\uF071", 40, y + 28);

  if (sub && sub[0]) {
    c.setFont(fontBody());
    c.setTextColor(C_WARN);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(title, 60, y + 9);

    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(sub, 60, y + 31);
  } else {
    c.setFont(fontBody());
    c.setTextColor(C_WARN);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(title, 60, y + 18);
  }
}

// Rain incoming alert banner.
static void drawRainBox(LGFX_Sprite &c, int y, bool withTemp) {
  const RainAlert &rain = rainAlertHome();
  c.fillRoundRect(8, y, 224, 44, 6, C_WARN);
  c.fillRoundRect(10, y + 2, 220, 40, 5, C_BG);

  char title[32];
  snprintf(title, sizeof(title), "RAIN IN ~%u MIN", (unsigned)rain.etaMin);
  c.setFont(fontBody());
  c.setTextColor(C_WARN);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(title, 18, y + 6);

  char sub[64];
  if (withTemp) {
    const WeatherNow &w = weatherNow();
    if (w.valid) {
      snprintf(sub, sizeof(sub), "%.1f mm/h from %s, %.0f\u00B0" "C now", rain.mmh, rain.from[0] ? rain.from : "?", (double)w.temp);
    } else {
      snprintf(sub, sizeof(sub), "%.1f mm/h from %s", rain.mmh, rain.from[0] ? rain.from : "?");
    }
  } else {
    snprintf(sub, sizeof(sub), "%.1f mm/h, moving in from %s", rain.mmh, rain.from[0] ? rain.from : "?");
  }
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(sub, 18, y + 25);
}

// GPU stat card (RX 6600 or RTX 3050).
static void drawGpuCard(LGFX_Sprite &c, int x, int y, const char *name, uint16_t acc,
                        double temp, double load, double mhz, double vram, double vramT) {
  drawCard(c, x, y, 114, 98);
  c.setFont(fontSmall());
  c.setTextColor(acc);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(name, x + 8, y + 6);

  char buf[32];
  if (isnan(temp)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0f\u00B0" "C", temp);
  }
  c.setTextColor(sev(temp, 75.0, 85.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 6);

  if (isnan(load)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", load);
  }
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, x + 8, y + 23);

  if (isnan(mhz)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0fMHz", mhz);
  }
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 28);

  drawBar(c, x + 8, y + 52, 98, 6, isnan(load) ? 0.0f : (float)(load / 100.0), acc);

  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("VRAM", x + 8, y + 62);

  if (isnan(vram) || isnan(vramT)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    if (fabs(vramT - round(vramT)) < 0.05) {
      snprintf(buf, sizeof(buf), "%.1f/%.0fG", vram, vramT);
    } else {
      snprintf(buf, sizeof(buf), "%.1f/%.1fG", vram, vramT);
    }
  }
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 62);

  float vramFrac = (isnan(vram) || isnan(vramT) || vramT <= 0.0) ? 0.0f : (float)(vram / vramT);
  drawBar(c, x + 8, y + 82, 98, 6, vramFrac, acc);
}

// Screen 1: PC stats face.
static void drawPcStats(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  // CPU card
  drawCard(c, 4, 4, 232, 66);
  c.setFont(fontSmall());
  c.setTextColor(C_CPU);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("CPU", 12, 9);

  double cpuLoad = pcStat("cpu_load");
  char buf[48];
  if (isnan(cpuLoad)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", cpuLoad);
  c.setFont(fontBig());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 24);

  double cpuClock = pcStat("cpu_clock");
  if (isnan(cpuClock)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.2f GHz", cpuClock / 1000.0);
  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 24);

  double cpuTemp = pcStat("cpu_temp");
  if (isnan(cpuTemp)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.1f\u00B0" "C", cpuTemp);
  c.setTextColor(sev(cpuTemp, 70.0, 85.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 44);

  drawBar(c, 12, 61, 216, 4, isnan(cpuLoad) ? 0.0f : (float)(cpuLoad / 100.0), C_CPU);

  // RAM card
  drawCard(c, 4, 76, 232, 52);
  c.setFont(fontSmall());
  c.setTextColor(C_RAM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("RAM", 12, 81);

  double ramUsed = pcStat("ram_used");
  double ramFree = pcStat("ram_free");
  if (isnan(ramUsed) || isnan(ramFree)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB", ramUsed, ramUsed + ramFree);
  }
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 81);

  double ramLoad = pcStat("ram_load");
  if (isnan(ramLoad)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", ramLoad);
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 98);

  drawBar(c, 72, 107, 156, 8, isnan(ramLoad) ? 0.0f : (float)(ramLoad / 100.0), C_RAM);

  // GPU cards (RX 6600 & RTX 3050, VRAM MB -> GB)
  double rxVram = pcStat("gpu1_vram");
  double rxVramT = pcStat("gpu1_vramt");
  drawGpuCard(c, 4, 134, "RX 6600", C_AMD,
              pcStat("gpu1_temp"), smoothedStat(0, "gpu1_load"), pcStat("gpu1_clock"),
              isnan(rxVram) ? NAN : rxVram / 1024.0, isnan(rxVramT) ? NAN : rxVramT / 1024.0);

  double rtxVram = pcStat("gpu2_vram");
  double rtxVramT = pcStat("gpu2_vramt");
  drawGpuCard(c, 122, 134, "RTX 3050", C_NV,
              pcStat("gpu2_temp"), smoothedStat(1, "gpu2_load"), pcStat("gpu2_clock"),
              isnan(rtxVram) ? NAN : rtxVram / 1024.0, isnan(rtxVramT) ? NAN : rtxVramT / 1024.0);

  // Fans card
  drawCard(c, 4, 238, 114, 78);
  c.setFont(fontSmall());
  c.setTextColor(C_FAN);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("FANS", 12, 244);

  c.setTextColor(C_FAINT);
  c.drawString("CPU", 12, 264);
  double cpuFan = pcStat("cpu_fan");
  if (isnan(cpuFan)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f rpm", cpuFan);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 110, 264);

  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("SYS", 12, 284);
  double sysFan = pcStat("sys_fan");
  if (isnan(sysFan)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f rpm", sysFan);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 110, 284);

  // Drive C: card
  drawCard(c, 122, 238, 114, 78);
  c.setFont(fontSmall());
  c.setTextColor(C_DISK);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("C:", 130, 244);

  double cUsed = pcStat("c_used");
  double cTotal = pcStat("c_total");
  double dp = (isnan(cUsed) || isnan(cTotal) || cTotal <= 0.0) ? NAN : (cUsed / cTotal);
  if (isnan(dp)) {
    snprintf(buf, sizeof(buf), "--");
    c.setTextColor(C_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", dp * 100.0);
    c.setTextColor(sev(dp * 100.0, 85.0, 95.0));
  }
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 244);

  if (isnan(cUsed) || isnan(cTotal)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f/%.0f GB", cUsed, cTotal);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 130, 264);

  drawBar(c, 130, 284, 98, 6, isnan(dp) ? 0.0f : (float)dp, C_DISK);

  if (isnan(cUsed) || isnan(cTotal)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f GB free", cTotal - cUsed);
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 130, 296);
}

// Screen 2: Media playback & uptime / network face.
static void drawMedia(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  bool hasRain = rainAlertHome().active;
  int top = hasRain ? 54 : 0;
  if (hasRain) {
    drawRainBox(c, 4, true);
  }

  // media.ok from the service only says the helper file is fresh; a session has a title
  bool mediaOk = (pcStat("media.ok") == 1.0) && pcText("media.title")[0];

  if (mediaOk) {
    int s = hasRain ? 104 : 144;
    int artX = 120 - s / 2;
    int artY = 8 + top;

    double artIdStat = pcStat("media.art_id");
    uint32_t artId = isnan(artIdStat) ? 0 : (uint32_t)artIdStat;
    const uint16_t *artPixels = artId ? artGet(artId) : nullptr;

    if (artPixels) {
      if (!hasRain) {
        c.pushImage(artX, artY, 144, 144, (const lgfx::swap565_t *)artPixels);
      } else {
        c.pushImageRotateZoom((float)artX, (float)artY, 0.0f, 0.0f, 0.0f,
                              104.0f / 144.0f, 104.0f / 144.0f, 144, 144,
                              (const lgfx::swap565_t *)artPixels);
      }
    } else {
      // Placeholder card with music note glyph
      c.fillRoundRect(artX, artY, s, s, 6, C_CARD);
      c.setFont(fontIconsSm());
      c.setTextColor(C_DIM);
      c.setTextDatum(lgfx::textdatum_t::middle_center);
      c.drawString("\uF001", 120, artY + s / 2);
    }

    int ty = 8 + top + s + 8;
    const char *title = pcText("media.title");
    drawMarquee(c, "media.title", title[0] ? title : "--", ty, fontHead(), C_TEXT);

    const char *artist = pcText("media.artist");
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    textImgDraw(c, "media.artist", artist[0] ? artist : "--", 120, ty + 26);

    // Source icon & name
    const char *src = pcText("media.source");
    uint16_t iconCol = C_DIM;
    const char *srcIcon = getMediaSourceIcon(src[0] ? src : "", iconCol);
    // one baseline for icons and text: the icon font's ascent is taller, so
    // top datums put the icons ~4 px low
    int rowBase = ty + 48 + 11;
    c.setFont(fontIconsSm());
    c.setTextColor(iconCol);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(srcIcon, 8, rowBase);

    c.setFont(fontSmall());
    c.setTextColor(iconCol);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    textImgDraw(c, "media.source", src[0] ? src : "--", 28, rowBase);

    // Local position advancing while playing
    static uint32_t lastSampleSeq = 0;
    static int lastSamplePos = 0;
    static uint32_t lastSampleMs = 0;

    uint32_t curSeq = pcStatsSeq();
    double posStat = pcStat("media.pos_s");
    double lenStat = pcStat("media.len_s");
    int pos = isnan(posStat) ? 0 : (int)posStat;
    int len = isnan(lenStat) ? 0 : (int)lenStat;
    bool playing = (pcStat("media.playing") == 1.0);

    if (curSeq != lastSampleSeq) {
      lastSampleSeq = curSeq;
      lastSamplePos = pos;
      lastSampleMs = nowMs();
    } else if (playing && len > 0) {
      uint32_t elapsed = (nowMs() - lastSampleMs) / 1000;
      pos = lastSamplePos + (int)elapsed;
      if (pos > len) pos = len;
    }
    if (pos < 0) pos = 0;

    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%d:%02d / %d:%02d", pos / 60, pos % 60, len / 60, len % 60);
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(timeBuf, 232, rowBase);

    int tw = c.textWidth(timeBuf);
    c.setFont(fontIconsSm());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(playing ? "\uF04B" : "\uF04C", 232 - tw - 4, rowBase);

    float pFrac = len > 0 ? ((float)pos / (float)len) : 0.0f;
    drawBar(c, 8, ty + 66, 224, 4, pFrac, C_TEXT);
  } else {
    // "Nothing playing" layout
    int y0 = top ? (top + 10) : 32;
    c.fillCircle(108, y0 + 64, 14, C_FAINT);
    c.fillCircle(140, y0 + 56, 14, C_FAINT);
    c.fillRect(118, y0 + 8, 4, 58, C_FAINT);
    c.fillRect(150, y0, 4, 58, C_FAINT);
    c.fillRect(118, y0, 36, 8, C_FAINT);

    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("Nothing playing", 120, y0 + 100);
  }

  // Uptime and Network throughput card
  drawCard(c, 4, 240, 232, 76);
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("UPTIME", 12, 246);

  char upBuf[32];
  formatUptime(pcStat("uptime"), upBuf, sizeof(upBuf));
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(upBuf, 12, 262);

  // Network down throughput
  drawNetArrow(c, 140, 251, true, C_CPU);
  char rateBuf[32];
  formatRate(pcStat("net_down"), rateBuf, sizeof(rateBuf));
  c.setFont(fontSmall());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(rateBuf, 228, 246);

  // Network up throughput
  drawNetArrow(c, 140, 271, false, C_RAM);
  formatRate(pcStat("net_up"), rateBuf, sizeof(rateBuf));
  c.drawString(rateBuf, 228, 266);

  // 56-sample net_down sparkline
  drawSparkline(c);
}

// Screen 2: Weather face (when PC is off/asleep/restarting/updating).
static void drawWeather(LGFX_Sprite &c) {
  const WeatherNow &w = weatherNow();
  c.fillScreen(C_WX_BG);

  if (!w.valid) {
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("No weather yet", 120, 140);
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("needs Wi-Fi", 120, 165);
    return;
  }

  // Location and update time
  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(w.place[0] ? w.place : "Home", 120, 6);

  char upBuf[32];
  snprintf(upBuf, sizeof(upBuf), "updated %s", w.updated[0] ? w.updated : "--");
  c.setFont(fontSmall());
  c.setTextColor(C_WX_TEXT_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(upBuf, 120, 26);

  // 44 px weather icon
  c.setFont(fontIcons());
  c.setTextColor(C_WX_CLOUD);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(getWxIconUtf8(w.icon, w.isDay), 120, 40);

  // 56 px temperature
  char tBuf[16];
  snprintf(tBuf, sizeof(tBuf), "%.0f\u00B0", (double)w.temp);
  c.setFont(fontDigits());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(tBuf, 120, 86);

  // Today High / Low
  char hlBuf[32];
  snprintf(hlBuf, sizeof(hlBuf), "%d\u00B0 / %d\u00B0", w.tmax, w.tmin);
  c.setFont(fontBody());
  c.setTextColor(C_WX_TEXT_MID);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(hlBuf, 120, 152);

  // Wind arrow pointing where the wind goes (windFromDeg + 180)
  float dirDeg = (float)(w.windFromDeg + 180);
  float theta = (dirDeg - 90.0f) * (float)(M_PI / 180.0f);
  float cx = 20.0f, cy = 186.0f;
  float x0 = cx + 8.0f * cosf(theta);
  float y0 = cy + 8.0f * sinf(theta);
  float x1 = cx + 6.0f * cosf(theta + 2.4f);
  float y1 = cy + 6.0f * sinf(theta + 2.4f);
  float x2 = cx + 6.0f * cosf(theta - 2.4f);
  float y2 = cy + 6.0f * sinf(theta - 2.4f);
  c.fillTriangle((int)roundf(x0), (int)roundf(y0),
                 (int)roundf(x1), (int)roundf(y1),
                 (int)roundf(x2), (int)roundf(y2), C_TEXT);

  static const char *dirs16[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                 "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  int dirIdx = (int)((w.windFromDeg + 11.25f) / 22.5f) % 16;
  char wBuf[32];
  snprintf(wBuf, sizeof(wBuf), "%.0f km/h %s", (double)w.windKmh, dirs16[dirIdx]);
  c.setFont(fontSmall());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(wBuf, 32, 180);

  // AQI chip
  if (w.aqi >= 0) {
    const char *lvl = "Low";
    uint16_t chipCol = C_GOOD;
    if (w.aqi > 200) { lvl = "Dangerous"; chipCol = C_PURPLE; }
    else if (w.aqi > 100) { lvl = "High"; chipCol = C_BAD; }
    else if (w.aqi > 50) { lvl = "Moderate"; chipCol = C_WARN; }

    char aqiBuf[32];
    snprintf(aqiBuf, sizeof(aqiBuf), "AQI %d %s", w.aqi, lvl);
    c.setFont(fontSmall());
    int tw = c.textWidth(aqiBuf);
    int chipW = tw + 16;
    if (chipW < 82) chipW = 82;
    int chipX = 232 - chipW;

    c.fillRoundRect(chipX, 177, chipW, 20, 10, chipCol);
    c.setTextColor(0x0000);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(aqiBuf, chipX + chipW / 2, 180);
  }

  // Rain forecast / radar box
  const RainAlert &rain = rainAlertHome();
  if (rain.active) {
    drawRainBox(c, 204, false);
  } else {
    c.fillRoundRect(8, 204, 224, 44, 6, C_WX_BOX_BG);
    c.setFont(fontSmall());
    c.setTextColor(C_WX_TEXT_MID);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString("No rain in the next 2 h", 18, 210);

    char radBuf[48];
    if (rain.dryKm > 0) {
      snprintf(radBuf, sizeof(radBuf), "radar: dry within %u km", (unsigned)rain.dryKm);
    } else {
      snprintf(radBuf, sizeof(radBuf), "radar: clear");
    }
    c.setTextColor(C_WX_TEXT_DIM);
    c.drawString(radBuf, 18, 227);
  }

  // 3-day forecast rows
  for (int i = 0; i < 3; i++) {
    int y = 256 + i * 21;
    int base = y + 11;   // shared baseline, as in the media row
    c.setFont(fontSmall());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(w.days[i].name, 16, base);

    c.setFont(fontIconsSm());
    c.setTextColor(C_WX_CLOUD);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(getWxIconUtf8(w.days[i].icon, true), 96, base);

    char dTemp[32];
    snprintf(dTemp, sizeof(dTemp), "%d\u00B0/%d\u00B0", w.days[i].tmax, w.days[i].tmin);
    c.setFont(fontSmall());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(dTemp, 224, base);

    if (i < 2) c.fillRect(16, y + 19, 208, 1, C_WX_DIV);
  }
}

// Screen 3: Claude & Antigravity quota / Local AI summary card.
static void drawAiUsage(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  // Claude card
  drawCard(c, 4, 4, 232, 130);
  c.setFont(fontSmall());
  c.setTextColor(C_CLAUDE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("CLAUDE", 12, 10);

  const char *cPlan = pcText("claude.plan");
  if (!strncmp(cPlan, "Claude ", 7)) cPlan += 7;   // the card already says CLAUDE
  drawChip(c, 228, 8, cPlan[0] ? cPlan : "Pro", C_CLAUDE, 0x0000, true);

  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Session", 12, 32);

  double sPct = pcStat("claude.s_pct");
  char buf[48];
  if (isnan(sPct)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", sPct);
  c.setFont(fontBody());
  c.setTextColor(sev(sPct, 75.0, 90.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 30);

  drawBar(c, 12, 52, 216, 6, isnan(sPct) ? 0.0f : (float)(sPct / 100.0), C_CLAUDE);

  const char *sReset = pcText("claude.s_reset_at");
  snprintf(buf, sizeof(buf), "resets %s", sReset[0] ? sReset : "--");
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 62);

  const char *sPace = pcText("claude.s_pace_v");
  uint16_t paceCol = (strcmp(sPace, "over") == 0) ? C_WARN : C_DIM;
  snprintf(buf, sizeof(buf), "pace %s", sPace[0] ? sPace : "--");
  c.setTextColor(paceCol);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 62);

  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Weekly", 12, 84);

  double wPct = pcStat("claude.w_pct");
  if (isnan(wPct)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", wPct);
  c.setFont(fontBody());
  c.setTextColor(sev(wPct, 80.0, 95.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 82);

  drawBar(c, 12, 104, 216, 6, isnan(wPct) ? 0.0f : (float)(wPct / 100.0), C_CLAUDE);

  const char *wReset = pcText("claude.w_reset_at");
  snprintf(buf, sizeof(buf), "resets %s", wReset[0] ? wReset : "--");
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 114);

  // Antigravity card: same layout as Claude's. The PC sends remaining
  // fractions; shown as used % so both cards read the same way.
  drawCard(c, 4, 138, 232, 130);
  c.setFont(fontSmall());
  c.setTextColor(C_AGY);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("ANTIGRAVITY", 12, 144);

  int agyN = 0;
  const char *active = pcText("agy.active");
  if (active && strchr(active, '@')) {
    for (int n = 1; n <= 4; n++) {
      char k[16]; snprintf(k, sizeof(k), "agy%d.email", n);
      if (strcmp(pcText(k), active) == 0) { agyN = n; break; }
    }
  }
  if (agyN == 0) {
    for (int n = 1; n <= 4; n++) {
      char k[16]; snprintf(k, sizeof(k), "agy%d.email", n);
      const char *em = pcText(k);
      if (em && em[0]) { agyN = n; break; }
    }
  }
  if (agyN == 0) agyN = 1;

  char k[32];
  snprintf(k, sizeof(k), "agy%d.plan", agyN);
  const char *aPlan = pcText(k);
  drawChip(c, 228, 142, aPlan[0] ? aPlan : "Pro", C_AGY, 0x0000, true);

  // flash_pct / flash_reset_at: the older single value is the 5-hour window
  char k5[32], k5r[32], kw[32], kwr[32];
  snprintf(k5, sizeof(k5), "agy%d.flash_5h_pct", agyN);
  snprintf(k5r, sizeof(k5r), "agy%d.flash_5h_reset_at", agyN);
  if (isnan(pcStat(k5))) {
    snprintf(k5, sizeof(k5), "agy%d.flash_pct", agyN);
    snprintf(k5r, sizeof(k5r), "agy%d.flash_reset_at", agyN);
  }
  snprintf(kw, sizeof(kw), "agy%d.flash_wk_pct", agyN);
  snprintf(kwr, sizeof(kwr), "agy%d.flash_wk_reset_at", agyN);

  struct Row { const char *label, *pctKey, *resetKey; double warn, bad; int y; };
  const Row rows[2] = {{"Session", k5, k5r, 75.0, 90.0, 166}, {"Weekly", kw, kwr, 80.0, 95.0, 218}};
  for (const Row &r : rows) {
    double left = pcStat(r.pctKey);
    double used = isnan(left) ? left : 100.0 - left;
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(r.label, 12, r.y);

    if (isnan(used)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%.0f%%", used);
    c.setFont(fontBody());
    c.setTextColor(sev(used, r.warn, r.bad));
    c.setTextDatum(lgfx::textdatum_t::top_right);
    c.drawString(buf, 228, r.y - 2);

    drawBar(c, 12, r.y + 20, 216, 6, isnan(used) ? 0.0f : (float)(used / 100.0), C_AGY);

    const char *rs = pcText(r.resetKey);
    snprintf(buf, sizeof(buf), "resets %s", rs[0] ? rs : "--");
    c.setFont(fontSmall());
    c.setTextColor(C_FAINT);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(buf, 12, r.y + 30);
  }

  // Local AI summary card
  drawCard(c, 4, 272, 232, 44);
  c.setFont(fontSmall());
  c.setTextColor(C_PURPLE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("LOCAL AI", 12, 278);

  const char *phaseName = pcText("ai.phase_name");
  if (!phaseName[0] || !strcmp(phaseName, "asleep")) phaseName = "Not Active";
  drawChip(c, 228, 276, phaseName, C_TRACK, C_DIM, true);

  const char *mName = pcText("ai.model_name");
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  textImgDraw(c, "ai.model_name", mName[0] ? mName : "Occamy-1.0", 12, 298);

  double reqs = pcStat("ai.requests");
  double errs = pcStat("ai.errors");
  char rBuf[48];
  snprintf(rBuf, sizeof(rBuf), "%.0f req, %.0f err", isnan(reqs) ? 0.0 : reqs, isnan(errs) ? 0.0 : errs);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(rBuf, 228, 298);
}

// Screen 3: Local AI full-screen working face.
static void drawLocalAi(LGFX_Sprite &c) {
  c.fillScreen(C_AI_BG);

  c.setFont(fontSmall());
  c.setTextColor(C_PURPLE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("LOCAL AI", 12, 8);

  bool pulse = ((nowMs() / 500) % 2) == 0;
  c.fillCircle(220, 16, 5, pulse ? C_PURPLE : C_AI_PULSE_DIM);

  const char *model = pcText("ai.model_name");
  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(model[0] ? model : "Occamy-1.0 35B-A3B", 12, 26);

  const char *phase = pcText("ai.phase_name");
  drawChip(c, 12, 50, phase[0] ? phase : "generating", C_PURPLE, 0x0000, false);

  double el = pcStat("ai.elapsed_s");
  uint32_t elSec = isnan(el) ? 0 : (uint32_t)el;
  char elBuf[32];
  snprintf(elBuf, sizeof(elBuf), "%lu:%02lu", (unsigned long)(elSec / 60), (unsigned long)(elSec % 60));
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(elBuf, 228, 51);

  double tps = pcStat("ai.gen_tok_s");
  char tpsBuf[16];
  if (isnan(tps)) snprintf(tpsBuf, sizeof(tpsBuf), "--");
  else snprintf(tpsBuf, sizeof(tpsBuf), "%.1f", tps);
  c.setFont(fontDigits());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(tpsBuf, 12, 70);

  c.setFont(fontBody());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString("tok/s", 228, 110);

  double step = pcStat("ai.step");
  double stepMax = pcStat("ai.step_max");
  char stepBuf[32];
  if (isnan(step) || isnan(stepMax)) snprintf(stepBuf, sizeof(stepBuf), "Step --");
  else snprintf(stepBuf, sizeof(stepBuf), "Step %.0f / %.0f", step, stepMax);
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(stepBuf, 12, 142);

  const char *tool = pcText("ai.tool");
  char toolBuf[48];
  snprintf(toolBuf, sizeof(toolBuf), "tool: %s", tool[0] ? tool : "--");
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(toolBuf, 228, 142);

  float sFrac = (isnan(step) || isnan(stepMax) || stepMax <= 0.0) ? 0.0f : (float)(step / stepMax);
  drawBar(c, 12, 160, 216, 6, sFrac, C_PURPLE);

  // Six detail cells
  struct CellInfo {
    const char *label;
    char val[24];
  };
  CellInfo cells[6];

  cells[0].label = "GPU";
  double gpuUtil = pcStat("ai.gpu_util");
  if (isnan(gpuUtil)) snprintf(cells[0].val, sizeof(cells[0].val), "--");
  else snprintf(cells[0].val, sizeof(cells[0].val), "%.0f%%", gpuUtil);

  cells[1].label = "CPU E-CORES";
  double ecore = pcStat("cpu_ecore_load");
  if (isnan(ecore)) snprintf(cells[1].val, sizeof(cells[1].val), "--");
  else snprintf(cells[1].val, sizeof(cells[1].val), "%.0f%%", ecore);

  cells[2].label = "VRAM (AI)";
  double vramAi = pcStat("ai.vram_mb");
  if (isnan(vramAi)) snprintf(cells[2].val, sizeof(cells[2].val), "--");
  else snprintf(cells[2].val, sizeof(cells[2].val), "%.1f GB", vramAi / 1024.0);

  cells[3].label = "RAM (AI)";
  double ramAi = pcStat("llm_ram_mb");
  if (isnan(ramAi)) snprintf(cells[3].val, sizeof(cells[3].val), "--");
  else {
    double ramGb = (ramAi >= 100.0) ? (ramAi / 1024.0) : ramAi;
    snprintf(cells[3].val, sizeof(cells[3].val), "%.1f GB", ramGb);
  }

  cells[4].label = "GPU TEMP";
  double gpuTemp = pcStat("ai.gpu_temp");
  if (isnan(gpuTemp)) snprintf(cells[4].val, sizeof(cells[4].val), "--");
  else snprintf(cells[4].val, sizeof(cells[4].val), "%.0f\u00B0" "C", gpuTemp);

  cells[5].label = "GPU+CPU";
  double gpuW = pcStat("ai.gpu_power_w");
  double cpuW = pcStat("cpu_power");
  if (isnan(gpuW) && isnan(cpuW)) snprintf(cells[5].val, sizeof(cells[5].val), "--");
  else {
    double totW = (isnan(gpuW) ? 0.0 : gpuW) + (isnan(cpuW) ? 0.0 : cpuW);
    snprintf(cells[5].val, sizeof(cells[5].val), "%.0f W", totW);
  }

  for (int i = 0; i < 6; i++) {
    int cx = 4 + (i % 2) * 118;
    int cy = 174 + (i / 2) * 48;
    c.fillRoundRect(cx, cy, 114, 44, 6, C_AI_CELL);
    c.setFont(fontSmall());
    c.setTextColor(C_FAINT);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(cells[i].label, cx + 8, cy + 4);

    c.setFont(fontBody());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(cells[i].val, cx + 8, cy + 20);
  }
}

// Screen 3: Big stacked clock face with PC status chip.
static void drawClock(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  GoingKind going = pcGoing();
  const char *chipText;
  uint16_t dotCol;
  if (going == GO_RESTART) {
    chipText = "PC Restarting";
    dotCol = C_WARN;
  } else if (going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN) {
    chipText = "PC Updating";
    dotCol = C_WARN;
  } else {
    chipText = "PC Offline / Disconnected";
    dotCol = C_BAD;
  }

  c.setFont(fontSmall());
  int tw = c.textWidth(chipText);
  int cw = tw + 28;
  int cx0 = 120 - cw / 2;
  c.fillRoundRect(cx0, 10, cw, 22, 11, C_CARD);
  c.fillCircle(cx0 + 12, 21, 4, dotCol);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(chipText, cx0 + 20, 13);

  struct tm tmNow;
  bool haveTime = boardLocalTime(&tmNow);
  if (haveTime) {
    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", tmNow.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tmNow.tm_min);

    c.setFont(fontClock());
    c.setTextColor(C_CLOCK_HOURS);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(hh, 120, 40);

    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(mm, 120, 148);

    static const char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char ds[32];
    snprintf(ds, sizeof(ds), "%s, %d %s", wdays[tmNow.tm_wday % 7], tmNow.tm_mday, months[tmNow.tm_mon % 12]);
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(ds, 120, 284);
  } else {
    c.setFont(fontDigits());
    c.setTextColor(C_CLOCK_HOURS);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 40);

    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 148);

    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 284);
  }
}

// ---- screens.h API Implementation ----

Face screensForce[N_PANELS];

Face screensFace(View v) {
  GoingKind going = pcGoing();
  bool restartish = (going == GO_RESTART || going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN);
  bool on = (pcState() == PC_ON && !restartish);

  switch (v) {
    case VIEW_PC:
      if (on) return FACE_PC_STATS;
      if (going == GO_RESTART) return FACE_GIF_RESTART;
      if (going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN) return FACE_GIF_UPDATE;
      return FACE_GIF;

    case VIEW_MEDIA:
      return on ? FACE_MEDIA : FACE_WEATHER;

    case VIEW_AI:
      return on ? (localAiBusy() ? FACE_LOCAL_AI : FACE_AI_USAGE) : FACE_CLOCK;

    default:
      return FACE_NONE;
  }
}

bool screensIsGif(Face f) {
  return f == FACE_GIF || f == FACE_GIF_RESTART || f == FACE_GIF_UPDATE;
}

void screensDraw(Face f, LGFX_Sprite &c) {
  switch (f) {
    case FACE_PC_STATS:
      drawPcStats(c);
      break;

    case FACE_GIF:
      // Plain GIF face: no canvas drawing needed (flush task copies pre-packed frames)
      break;

    case FACE_GIF_RESTART:
      drawWarnBox(c, "PC is Restarting", nullptr);
      break;

    case FACE_GIF_UPDATE: {
      char sub[48];
      uint32_t mins = pcGoingAgeMs() / 60000;
      snprintf(sub, sizeof(sub), "installing, %lu min so far", (unsigned long)mins);
      drawWarnBox(c, "PC is Updating", sub);
      break;
    }

    case FACE_MEDIA:
      drawMedia(c);
      break;

    case FACE_WEATHER:
      drawWeather(c);
      break;

    case FACE_AI_USAGE:
      drawAiUsage(c);
      break;

    case FACE_LOCAL_AI:
      drawLocalAi(c);
      break;

    case FACE_CLOCK:
      drawClock(c);
      break;

    default:
      break;
  }
}

uint32_t screensPeriodMs(Face f) {
  switch (f) {
    case FACE_PC_STATS:
    case FACE_GIF:
    case FACE_GIF_RESTART:
    case FACE_GIF_UPDATE:
    case FACE_AI_USAGE:
      return 0;

    case FACE_MEDIA:
      return mediaMarqueeMoving() ? 40 : 1000;

    case FACE_WEATHER:
      return 5000;

    case FACE_LOCAL_AI:
      return 250;

    case FACE_CLOCK:
      return 1000;

    default:
      return 0;
  }
}

bool localAiBusy() {
  if (pcStat("ai.ok") != 1.0) return false;
  if (pcStat("ai.active") == 1.0) return true;
  double phase = pcStat("ai.phase");
  if (isnan(phase)) return false;
  int p = (int)phase;
  return p == 1 || p == 3 || p == 4 || p == 5 || p == 6;
}
