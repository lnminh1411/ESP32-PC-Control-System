#pragma once

// PC usage warnings for the Media face: RAM, CPU and GPU load and the
// temperatures of the CPU, both GPUs, the RAM and the motherboard VRM, each
// judged on its average over the last 2 s of PC samples.
//
//   RAM load   > 80 %  yellow, > 90 % red (critical)
//   CPU load   > 90 %  yellow
//   GPU 1 / 2  > 90 %  yellow (RX 6600, RTX 3050)
//   RX 6600 VRAM > 90 % used  yellow (gpu1_vram_pct, worked out from gpu1_vram / gpu1_vramt)
//   temps      red above 80 C (cpu_temp, gpu1_temp) or 75 C (gpu2_temp, ram_temp, vrm_temp)
//
// Defaults; `pcwarn set` / `pcwarn avg` change them (saved in NVS).
//
// The Media face shows every active one in one box, red when any is red,
// in place of the weather warning. The PC face tints each card (CPU, RAM,
// RX 6600, RTX 3050) with the worst level of its own metrics.

#include <stdint.h>

enum PcWarnLevel : uint8_t { PCWARN_NONE = 0, PCWARN_HIGH = 1, PCWARN_CRITICAL = 2 };

struct PcWarn {
  const char *label;   // "RAM", "CPU", "GPU1", "GPU2", "VRM"
  float value;         // the average, % or C
  float limit;         // the threshold it is over
  bool temp;           // a temperature warning (else a load one)
  PcWarnLevel level;
};

#define PCWARN_MAX 10

void pcWarnPoll();                     // main loop: takes each new PC sample
int pcWarnList(PcWarn *out);           // active warnings, critical first; out holds PCWARN_MAX
PcWarnLevel pcWarnLevel(const char *key);   // one metric's level, e.g. "cpu_temp"; PC face cards
uint32_t pcWarnAvgMs();                // the averaging window
void pcWarnCommand(const char *args);  // console `pcwarn ...`, the whole rest of the line
