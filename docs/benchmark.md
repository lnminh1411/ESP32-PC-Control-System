# Benchmark

Carrier board (ESP32-S3 rev 0.2, 240 MHz, 16 MB flash, octal PSRAM at 40 MHz
with ECC, 7680 KB usable), firmware as of 2026-11-01, PC on, Wi-Fi at -18 dBm.
The dated sections below were measured at the time, the ones before
2026-10-28 with PSRAM at 80 MHz and without ECC (why it changed:
[firmware.md](firmware.md#psram-hardening)).
Run it again with the console command `bench all` (`bench usage` for memory use only,
`bench usb <KB>` for USB),
and time Wi-Fi from the PC with
`curl -o NUL -w "%{speed_download}\n" http://<board-ip>/bench?kb=4096`.

## Usage

From `bench usage` (part of `bench all`), PC on, 62-frame GIF pack inflated as
it plays, one panel having shown a GIF. Each panel that shows one holds 75 KB
of PSRAM.

| Memory | Used | Notes |
|---|---|---|
| SRAM (512 KB) | 446 KB (87%) | 173 KB static (code in IRAM, data/bss, ROM, cache); heap 338 KB; lowest free ever: 25 KB, largest block 30 KB |
| PSRAM (7680 KB) | 1585 KB (21%) | lowest free ever 5017 KB, largest free block 6015 KB |
| Flash (16 MB) | fully partitioned | app slots 2 x 5 MB (OTA), FAT 5.9 MB, NVS 20 KB, core dump 64 KB |
| App image | 2848 KB of 5120 KB (56%) | 1201 KB of it is the GIF pack |
| FAT /ffat | 4 KB of 5932 KB (0%) | layout.json, saved text images |
| NVS | 229 of 630 entries | 12 namespaces |

Task stacks, least ever free (bytes): main 6572, discord 5488, httpd 4904,
websocket 4608, weather 3444, TinyUSB 2872, flush 2816, wifi 2528, esp_timer 2056,
mdns 1396, Tmr Svc 1332, tiT 1312, mqtt 1232, led 1204, heapguard 796, sys_evt 784,
IDLE 692-700, ipc 496-504.

## Internal RAM, before and after 2026-10-26

Internal RAM was the tight one: free space fell to ~3 KB within 8 s of boot,
when the Wi-Fi login and three or four TLS handshakes ran at once, and
idled near 40 KB with an 8.7 KB largest block (a TLS handshake needs ~4 KB
blocks). Four changes:

1. Wi-Fi static TX buffers 16 -> 8 (`CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM`):
   with PSRAM on they default to static, ~1.6 KB each in internal RAM for good.
2. mbedTLS: dynamic buffers, config data and CA chain freed after the
   handshake, peer certificate not kept. (Dynamic buffers were turned off
   again on 2026-10-27; see below.)
3. Discord button clicks and slash commands handled in the bot task through a
   queue instead of in the websocket task: one REST TLS client instead of two,
   websocket task stack 12 -> 8 KB, and a slow PC command no longer holds the
   gateway connection.
4. The weather task waits for Discord's gateway (or 30 s) before its first
   HTTPS fetch after Wi-Fi comes up.

Same test both times: three rounds of `bench all`, `bench usb 2048`, three 4 MB
Wi-Fi downloads and 30 pings (`soak.py`).

| | Before (internal lwIP, 18:26) | After (21:00) |
|---|---|---|
| Internal free, lowest during the rounds | 40.9 KB | 73.1 KB |
| Largest internal block | 8.7 KB | 31.7 KB |
| Internal free, lowest ever since boot | ~2-3 KB | 34 KB |
| PSRAM free | 3.16 MB | 3.23 MB |
| Wi-Fi download, 9 runs | 0.60-0.88 MB/s, 0.80 average | 0.62-0.92 MB/s, 0.78 average |
| `bench usb 2048` CDC write | 0.23 MB/s | 0.23 MB/s |
| Ping loss | 0% | 0% |

Step by step, 90 s after boot: 40 KB free / 7.5 KB largest -> 54.5 / 22.5
(TX buffers) -> 68.7 / 29.7 (mbedTLS) -> 72.5 / 31.7 (queue; lowest ever 22 ->
38 KB) -> 72.6 / 31.7 (boot stagger, kept as a guard). The Wi-Fi difference is
within the radio's run-to-run swing.

## USB buffers, cache lines, one draw per pass (2026-10-26, later)

Three changes:

1. USB receive stream buffers (HID 24 KB, CDC 4 KB) store their bytes in
   PSRAM. FreeRTOS allocates them from internal RAM by default. The control
   blocks stay internal.
2. Data cache lines 32 -> 64 bytes (`CONFIG_ESP32S3_DATA_CACHE_LINE_64B`). The
   faces draw into PSRAM canvases, so most of their time is PSRAM traffic.
3. `viewsService` draws at most one canvas per pass. With three static faces
   due at once it drew all three back to back (20 + 19 + 30 ms), and the
   console waited up to 74 ms. Now the console waits for one face at most.
   GIF frames need no draw, so they still go in the same pass.

The task watchdog came back too: the main loop is subscribed, and a stall
panics after 20 s (core dump, reboot).

Wi-Fi was down on both runs (the 2.4 GHz network was off the air), so the
internal RAM numbers below are lower in use than with Discord and weather
running. Compare the difference, not the totals, with the tables above.

| | Before (HEAD 4112843) | After |
|---|---|---|
| Internal free | 133.1 KB | 162.6 KB |
| Largest internal block | 58 KB | 88 KB |
| Internal free, lowest ever since boot | 93 KB | 120 KB |
| PSRAM free | 3249 KB | 3220 KB |
| Main task CPU (core 1, faces pc/media/ai at 2 fps) | 24.2% | 20.9% |
| Worst main loop pass | 72-75 ms | one face, 27 ms at most |

Face draw time per frame, measured with a temporary profiler (30 frames each):

| Face | 32-byte lines | 64-byte lines |
|---|---|---|
| PC stats | 20.6 ms | 18.4 ms |
| Media | 19.3 ms | 14.8 ms |
| AI usage | 29.7 ms | 25.1 ms |

`fillScreen` alone takes 3-7 ms per face, and each card fills its area again.
Text is about 1 ms per string. The time is spent on pixel writes to PSRAM, so
further cuts need partial redraws (only the fields that changed).

## WinUSB bulk pipe and USB receive backpressure (2026-10-26, later)

1. The board gets a vendor interface with two bulk endpoints. Its MS OS 2.0
   descriptors make Windows bind WinUSB with no driver install, and the
   service uses it before HID.
2. USB serial (CDC) and the new pipe stop dropping bytes when the board's
   receive buffer is full: the bytes wait in TinyUSB's FIFO and the PC is
   told to wait (NAK). Before, a 41 KB burst over COM12 lost lines every time.

| | Before | After |
|---|---|---|
| Cover art (41 KB of base64) from the service, board's own timing | 688 ms (HID) | 115 ms (WinUSB) |
| 43 KB burst to COM12 with no pacing | lines lost (`art: bad chunk`) | intact, 0.4 MB/s |
| Internal RAM free | 162.4 KB | 158 KB |
| Largest internal block | 88 KB | 80 KB |

The pipe is no longer the limit: 115 ms is the board parsing the base64 text
lines one byte at a time, the same as for the burst over COM12 (117 ms).

## Speed

| Area | Test | Result |
|---|---|---|
| CPU | 32-bit integer multiply-add | 79 M/s (3 cycles each) |
| CPU | float multiply-add (FPU) | 79 MFLOPS |
| CPU | double multiply-add (software) | 3 MFLOPS |
| SRAM | copy / write / read | 239-377 / 238-738 / 119 MB/s (varies run to run with alignment) |
| SRAM | random read | 13 ns |
| PSRAM | copy / write / read (1 MB buffers) | 15 / 23 / 41 MB/s (31 / 48 / 72 at 80 MHz) |
| PSRAM | random read (1 MB span) | 1410-1420 ns (697 at 80 MHz) |
| PSRAM | stress, 4 MB x 8 passes | 0 bad words, 2.5 s |
| PSRAM to SRAM | copy in 4 KB pieces | 43 MB/s (86 at 80 MHz) |
| Flash | raw partition read (4 KB) | 10.8 MB/s |
| Flash | memory-mapped read (cache) | 27.5-30.4 MB/s |
| Display SPI | full 240x320 RGB444 frame (115 200 B) | 32.9-37.2 ms, 3.1-3.5 MB/s, 62-70% of the 40 MHz wire (27.8-28.9 ms, 4.0-4.1 MB/s with PSRAM at 80 MHz) |
| USB CDC | board to PC | 0.70 MB/s (`bench usb 2048`) |
| USB CDC | PC to board, text lines parsed | 0.4 MB/s (43 KB in 105 ms, 2026-10-26) |
| USB | cover art, PC service to board | 115 ms on WinUSB bulk, 688 ms on HID (2026-10-26) |
| Wi-Fi | HTTP download to the PC (4 MB, 3 runs) | 0.84-0.89 MB/s |
| Wi-Fi | ping | 2-5 ms, 3 ms average |

Notes:
- A full frame takes ~35 ms per panel, so all three panels repainting at once
  cap out near 10 fps; the dirty-rect diff is what keeps normal faces at their
  frame rates. SPI runs at 40 MHz because 80 MHz gave blank panels.
- `bench spi` reads the canvas from PSRAM. PSRAM went from 80 to 40 MHz on
  2026-10-28, the likely cause of the slower frame (not measured on its own).
- Wi-Fi throughput is low on purpose: 8 static RX and 8 static TX buffers and
  a BA window of 8 free internal RAM for TLS (see `sdkconfig.defaults`).
  Nothing on the board needs more than ~100 KB/s.

## Build changes and TLS dynamic buffers off, 2026-10-27

The build changes (MINIMAL_BUILD, lovyangfx driver requirements, cached IDF
environment) were measured against the main build. Both builds carried the
heapguard debug checker and light heap poisoning.

With the build changes in, the board crash-looped about 50 s after each boot.
Closing the GDACS cyclone download (200-350 KB over TLS) freed a corrupted
mbedTLS dynamic-buffer block in internal RAM: 2 bytes were written past its
end. `CONFIG_MBEDTLS_DYNAMIC_BUFFER` is now off.

| | Before (main) | After (build changes, dynamic buffers off) |
|---|---|---|
| Internal free during the rounds | 75.5 KB | 77.9 KB |
| Internal free, lowest ever since boot | 22 KB | 34 KB |
| Largest internal block | 31.7 KB | 31.7 KB |
| PSRAM free | 3.17 MB | 3.11 MB |
| App image | 1591 KB | 1586 KB |
| Core 1 (main loop) | 22.8 % | 21.8 % |
| Wi-Fi download, 9 runs | 0.99-1.14 MB/s | 0.44-0.97 MB/s, one stall (radio) |
| `bench usb 2048` CDC write | 0.23 MB/s | 0.14-0.22 MB/s (varies run to run) |
| Incremental build | 33-42 s | 16-23 s |
| Clean build | 269 s | 158-197 s |
| Crashes | 0 | 0, through both cyclone fetches |

## GIFs built into the firmware, 2026-11-01

The GIFs moved from an upload in FAT, decoded at boot into whole RGB444 frames,
to `gifs.bin` in the app image: per-GIF palettes, frame 0 whole and the other
frames as their dirty rects, zlib-compressed in flash and inflated into PSRAM
at boot. Both GIFs now play at 20 fps (Neuro 22 frames, Evil Neuro 20, 50 ms
each); the first pack had 27 frames at 15 and 10 fps. "Before" is the previous
firmware with its single test GIF, and its ~3.8 MB is an estimate.

| | Before | First pack, 27 frames | Now, 42 frames |
|---|---|---|---|
| GIF pack | none | 532 KB | 793 KB |
| Frames in PSRAM | ~3.8 MB with tables and buffers | 1780 KB | 2722 KB |
| PSRAM used, `bench usage`, three panel buffers (75 KB each) | 5179 KB | 3428 KB | 4374 KB |
| PSRAM free, lowest ever without bench buffers | 1438 KB | 3426 KB | 2456 KB |
| App image | 1650 KB | 2178 KB | 2439 KB |
| FAT /ffat | 4508 KB | 4 KB | 4 KB |
| Decoding at boot | 4.5 MB read from FAT | 329 ms to inflate | 498 ms to inflate |

Dirty rects cover most of the screen on most frames (86-95% before the Neuro
rebuild), since most of each picture moves, which is why the frames take this
much PSRAM.

| GIF playback at 20 fps | Frame rate | Frame times | Bus |
|---|---|---|---|
| One panel | 19.5 fps (17.7-20.5) | 1.04x nominal | 1.9 MB/s, 41% |
| Two panels | 17.8 fps | 1.12x nominal | 70% |
| Three panels | 12.7 fps | 1.67x nominal | 73% |

A frame takes 26-28 ms to push. The GIF slows down rather than drop frames, so
on three panels it plays at about 60% speed; three panels at 20 fps need about
5.7 MB/s, more than the 40 MHz wire carries.

The app slots then grew from 3 MB to 5 MB each and FAT shrank from 9.9 MB
to 5.9 MB (`partitions.csv`, flashed over USB). The same image now fills 48%
of its slot instead of 79%, leaving about 2.6 MB for more GIFs.

### Frames inflated as they play

The frames then stopped being inflated into PSRAM at boot. They stay
compressed in flash, and the `gifdec` task (core 1) inflates each one just
before the flush task pushes it. Measured with the 42-frame pack, PC on, GIF
on panel 1:

| | Inflated at boot | Inflated as they play |
|---|---|---|
| PSRAM used (`bench usage`) | 4374 KB (three panel buffers) | 1584 KB (one panel buffer) |
| PSRAM lowest free | 2456 KB | 5094 KB |
| Frames in PSRAM | 2722 KB | none: 113 KB of tables and one 75 KB inflate buffer |
| SRAM lowest free | 29 KB | 25 KB |
| Boot | 498 ms to inflate | none |
| Frame cap | 64 | 512, and 8 GIFs |

| Inflating | Core 1 | Panel 1 fps |
|---|---|---|
| `gifdec` at priority 0 (below main) | 80% | 9.8: it shared only the idle time, with IDLE1 |
| `gifdec` at main's priority (1) | 66-92% | 19.4-20.0 |

- **Per frame:** 15-16 ms of CPU. The inflate state sits in PSRAM: in internal
  RAM it was no faster and took 11 KB of the scarce SRAM (lowest free fell to
  14 KB).
- **CPU:** a 20 fps GIF takes about 30% of core 1. Core 1 reached 92% only
  with the media face also redrawing at 17 fps; with the other faces at 2 fps
  it sits near 66%.
- **Three panels:** 11-13 fps per panel, the same as before: the SPI bus is
  the limit, not inflating.
- **March 7th** then joined as a third GIF: 1 s of its 30 fps source (frames
  120-149, a loop that closes), RIFE-interpolated to 20 frames at 20 fps. The
  pack is 1201 KB for 62 frames, and the app image fills 56% of its slot.
