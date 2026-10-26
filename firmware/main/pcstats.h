// PC stats from the host service (host/pc_stats).
// Configured stats are streamed over vendor HID (report ID 6, 64-byte reports)
// with USB CDC serial fallback as ASCII lines:
//   @s key=value key=value ...     (numbers, split into lines <= 300 chars)
//   @t key text                    (text values up to 80 chars)
//   @e <seq>                       end of one sample (every 500 ms by default)
//
// Keys and mappings are defined by stats.ini on the host PC.
// ai.* keys come from the local status hub (see E:\Local LLM\StatusHub.md
// for key list and phase/guard codes).
//
// Shipped keys and units:
//   uptime:     seconds (s)
//   cpu_temp:   degrees Celsius (C)
//   cpu_load:   load percentage (%)
//   cpu_clock:  megahertz (MHz)
//   cpu_fan:    revolutions per minute (RPM)
//   sys_fan:    revolutions per minute (RPM)
//   ram_load:   load percentage (%)
//   ram_used:   gigabytes (GB)
//   ram_free:   gigabytes (GB)
//   gpu1_temp:  degrees Celsius (C)
//   gpu1_fan:   revolutions per minute (RPM)
//   gpu1_clock: megahertz (MHz)
//   gpu1_load:  load percentage (%)
//   gpu1_vram:  megabytes (MB)
//   gpu1_vramt: megabytes (MB)
//   gpu2_temp:  degrees Celsius (C)
//   gpu2_fan:   revolutions per minute (RPM)
//   gpu2_clock: megahertz (MHz)
//   gpu2_load:  load percentage (%)
//   gpu2_vram:  megabytes (MB)
//   gpu2_vramt: megabytes (MB)
//   igpu_load:  load percentage (%)
//   c_used:     gigabytes (GB)
//   c_total:    gigabytes (GB)
//   net_down:   bytes per second (bytes/s)
//   net_up:     bytes per second (bytes/s)
#pragma once
#include <stdint.h>

#define PCSTATS_MAX 160
#define PCSTATS_TEXT_MAX 64   // media.* and pc.going* pushed past the old 24

void pcStatsLine(const char *line);   // console hands '@' lines here (already wired in console.cpp)
double pcStat(const char *key);        // NAN if unknown, empty, or no sample for 5 s
bool pcStatsFresh();                  // an "@e" arrived in the last 3 s
uint32_t pcStatsSeq();                // increments on every @e (end of a sample)
int pcStatsCount();
const char *pcStatKey(int i);         // for listing
double pcStatAt(int i);               // raw last value (NAN = empty)

const char *pcText(const char *key);  // "" if unknown or no sample for 5 s
int pcTextCount();
const char *pcTextKey(int i);
const char *pcTextAt(int i);
