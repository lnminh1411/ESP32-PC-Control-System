# Firmware

`firmware/` is plain ESP-IDF 5.5.6 with LovyanGFX (a git submodule
behind a wrapper component) for drawing. Each panel draws into a PSRAM
sprite; the transport underneath is esp_lcd with RGB444 on the wire.
Building and flashing: [build-and-flash.md](build-and-flash.md). Settings:
[settings.md](settings.md).

## Modules

| Module | Does |
|---|---|
| `config.h`, `display_config.h` | Pins, timings, panel table |
| `display` | Panel init, flush task on core 0, dirty-rectangle engine, RGB444 push over SPI + DMA, backlights, SPI budget |
| `views` | What each panel shows (`pc`, `media`, `ai`, `gif`, `dash`, test views), saved in NVS |
| `screens` | The faces ([screens.md](screens.md)) and which one each view picks |
| `layout` | JSON dashboard pages from `/ffat/layout.json` ([layouts.md](layouts.md)) |
| `pcstats`, `textimg`, `art` | Stats the PC sends; PC-rendered text bitmaps; cover art |
| `pcwarn` | PC usage warnings: 2 s averages against limits saved in NVS |
| `pc_state`, `pc_going` | PC power state from USB events; restart/update detection; status LED |
| `optos`, `power`, `wol` | Door presses and the reset pulse; USB power commands; Wake-on-LAN (3 bursts to ports 9 and 7, MAC learned from the PC helper) |
| `night` | Night mode and low power mode ([power-and-night.md](power-and-night.md)) |
| `hwctl`, `aictl`, `monctl` | `!hw`, `!ai` and `!mon` relays through the PC helper: case RGB and fans (PcHwCtl), the local AI stack (LocalAIHost), monitors (PcHwCtl) |
| `weather`, `nowcast`, `owm`, `cyclone`, `owntracks`, `pushover` | Open-Meteo forecast and AQI, rain nowcast and warnings, OpenWeather One Call 4.0 minute-by-minute rain and alerts, GDACS typhoon tracking, phone location over MQTT, Pushover copies of the PC and weather announcements ([weather.md](weather.md)) |
| `board_time` | Wall clock: SNTP over Wi-Fi, else the time the PC sends |
| `net`, `discord` | Wi-Fi (TX power capped, self-healing rejoin), mDNS, HTTP server (`/ota`, `/layout`, `/bench`); Discord bot with an `/actions` panel and a standing #control panel ([discord.md](discord.md)) |
| `gif_store` | GIFs built into the firmware (`gifs.bin`, from `scripts/gif_pack.py`): palette frames, each storing only its dirty rects, kept compressed in flash and inflated as they play |
| `commands`, `console`, `usb`, `host_uart`, `log` | The text command set ([console.md](console.md)); USB CDC, CH343 UART (3 Mbaud), WinUSB bulk and vendor-HID transports; the log fan-out (UART, CDC, Discord #logging) |
| `sys`, `heapguard`, `bench` | Reset reason and core dump summary at boot, brownout settle, PSRAM ECC log; debug heap guard (internal heap check every 20 ms, ROM printf ring kept across resets, socket-VFS watchpoints); `bench` tests |

## Display pipeline

- **Dirty rectangles:** the flush task compares each canvas with what is
  already on the glass in 8-row bands and sends only the changed
  rectangles, so a changed number costs about 1 KB and a static face costs
  nothing.
- **Event-driven redraw with an SPI budget:** faces redraw when their data
  changes or their own timer is due (the marquee at 25 fps, the clock once
  a second), and a token bucket keeps the shared bus under 85%. GIFs slow
  down instead of dropping frames when the bus is busy. `fps` shows and
  tunes it.
- **RGB444 on the wire** (12 bits per pixel, 25% less data than RGB565).
- **Two cores:** core 1 draws, core 0 diffs and sends. A GIF on one panel
  keeps its own frame times (20 fps, 1.9 MB/s).

## Other behaviour

- **Door buttons B1-B4** (GPIO39-42): 500 ms presses, one at a time, with a
  500 ms gap enforced in firmware (the board has no hardware interlock).
- **Motherboard reset** (GPIO1): 200 ms pulse, only on the `reset` command,
  never at boot. All opto pins are driven LOW first thing at boot.
- **PC power state** from the module's own USB link, with a 30 s guard
  against the brief USB dropout at power-on. While any panel is on an auto
  view (`pc`, `media`, `ai`) the backlights stay on and show the PC-off
  faces; with none on an auto view they turn off when the PC sleeps or
  shuts down.
- **Brownout settle:** a boot after a brownout waits 3 s, then restarts once
  the supply is steady, so PSRAM starts on a clean rail. The log names it as
  the restart reason. A boot after a panic or
  watchdog reset logs the core dump summary (task, PC, cause, backtrace).
- **Status LED** (GPIO2, PWM). `led <0-100>` is perceptual brightness,
  capped at 35% duty.

  | LED | Means |
  |---|---|
  | Quick pulses, 100 ms | Loading: the board is booting, or the PC is on but its stats have not arrived yet |
  | Solid | Board and PC online |
  | Quick pulses, 200 ms | PC restarting or updating |
  | Slow breathing (3 s fade in and out) | PC asleep or off |

## Tasks

| Task | Core | Stack | Does |
|---|---|---|---|
| main | 1 | 10 KB | Draws the faces; console, USB, power, night and the rest of the main loop (20 s task watchdog) |
| `flush` | 0 | 4 KB | Diffs each canvas against the glass, sends the dirty rectangles |
| `gifdec` | 1 | 3 KB | Inflates each GIF frame from flash into the panel's frame buffer, then hands it to `flush` (main's priority: about 30% of core 1 for a 20 fps GIF) |
| `led` | 0 | 2 KB | Status LED patterns |
| `discord` | 0 | 10 KB | Discord gateway and REST |
| `weather` | 0 | 9 KB | Open-Meteo, OpenWeather, GDACS; the Pushover queue |
| `heapguard` | any | 3 KB | Debug: internal heap check every 20 ms, socket-VFS pointer check |

## ESP-IDF configuration

`firmware/sdkconfig.defaults` holds the board's ESP-IDF choices: 240 MHz,
16 MB QIO flash, octal PSRAM at 40 MHz with ECC (below), Wi-Fi, lwIP and the
VFS select tables kept in internal RAM, a 20 s task watchdog that panics, ELF
core dumps to flash, app rollback, and the console on the CH343 UART at
3 Mbaud. The partition table (`firmware/partitions.csv`) has two 5 MB app
slots (the GIF pack is in the app image), a 5.9 MB FAT partition (`/ffat`:
layout, pinned text) and a 64 KB core dump partition. A partition table
change only reaches the board over USB (`idf_flash.py`), not OTA; moving
`/ffat` reformats it.

## PSRAM hardening

PSRAM runs at 40 MHz with ECC. Until 2026-10-28 it ran at 80 MHz without
ECC, and from 10-25 to 10-28 the board kept crashing.

**The crashes.** Every one hit data kept in PSRAM: a lwIP timer pointer with
one bit flipped (`0x3c6dda68` read back as `0x3d6dda68`), free-list pointers
in the PSRAM heap, TLS records failing their MAC check, and the socket VFS
function pointers. Most were cache errors (exccause 0x47). The heap canaries
next to the damage were intact, a pattern test (`bench psram 2`) found no bad
words with the board idle, and two code sweeps fixed 11 bugs but none that
wrote into those structures.

**The bitflip.** At 14:43 on 10-28 the PC was woken, and the board browned
out and restarted. The new run's first message, "Board restarted after
brownout (supply voltage dropped)", reached Discord as "Boird resuasted agter
browno}t ({upply)vom|age!droypee)". Each wrong letter is the right one with a
single bit turned from 0 to 1. The string is correct in flash, so its copy in
PSRAM had gone bad. Seconds later heapguard found three PSRAM canaries
overwritten, one with the start of a Discord gateway message, and the board
crashed. The cause is electrical: the board runs from the motherboard's 5 V,
which sags as the PC powers on ([power-and-night.md](power-and-night.md#power-on-brownout))
and dips under load.

**Why 40 MHz.** At 80 MHz, ESP-IDF tunes the MSPI read timing at every boot.
A boot after a brownout runs that tuning while the supply is still low, and
the board keeps that timing for the whole run, which fits the garbled boot.
At 40 MHz there is no tuning (IDF only tunes above 40 MHz) and each bit has
twice the time on the bus, so a dip in the supply has more margin before reads
go wrong. That matters because some crashes came 7 to 20 minutes into a run
with the PC on and no power event, which a bad boot tuning does not explain.

**The rest of the fix** (commit f366c8b):
- ECC corrects single-bit errors. 512 KB of the 8 MB holds the check bits,
  leaving 7680 KB. `sys.cpp` reads the hardware's corrected-error counter
  every 10 s and logs `psram: ECC corrected ...` with the PC state.
- A brownout boot restarts once more after 3 s (above).
- The VFS select tables live in internal RAM.
- Wi-Fi TX power is capped at 8 dBm to draw less from the shared 5 V.

**Cost:** PSRAM bandwidth roughly halves (copy 15 MB/s instead of 31), and a
full panel frame takes 33-37 ms instead of 28-29 ms. The numbers are in
[benchmark.md](benchmark.md).
