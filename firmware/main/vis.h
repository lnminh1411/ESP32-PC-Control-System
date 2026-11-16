// Music-reactive cover border: 6 band lengths and 6 band volumes from the PC
// (HID output report 7, [0x56 'V'][seq][6 lengths][6 volumes], 30 a second
// while audio plays) and the counters
// that benchmark the stream. The flush task draws the ring (display.cpp).
#pragma once
#include <stdint.h>

constexpr int VIS_BANDS = 6;
constexpr int VIS_CH = 2 * VIS_BANDS;   // lengths, then volumes

void visPut(uint8_t seq, const uint8_t *ch);   // TinyUSB task: one report 7 arrived (VIS_CH bytes)
// What the ring shows now, 0..1: len[b] = the band's share of the ring,
// vol[b] = its brightness against the song. Each: the last 100 ms of frames
// averaged, a value held 100 ms, then falling no faster than full scale in
// 0.5 s (a frame older than 250 ms counts as silence). false = all dark.
bool visLevels(float len[VIS_BANDS], float vol[VIS_BANDS]);
bool visEnabled();                                  // ring on (NVS "vis"/"on", default on)
void visConsole(const char *args);                  // vis [on|off|stats|reset]
void visRingPushed(uint32_t us, uint32_t gapMs);    // flush task: one ring frame went out
