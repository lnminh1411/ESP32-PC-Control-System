# Benchmark

Carrier board (ESP32-S3 rev 0.2, 240 MHz, 16 MB flash, 8 MB octal PSRAM at
80 MHz), firmware as of 2026-10-26, PC on, Wi-Fi at -27 dBm.
Run it again with the console command `bench all` (`bench usage` for memory use only,
`bench usb <KB>` for USB),
and time Wi-Fi from the PC with
`curl -o NUL -w "%{speed_download}\n" http://<board-ip>/bench?kb=4096`.

## Usage

From `bench usage` (part of `bench all`), after three bench rounds (see below), PC on.
Taken before the USB stream buffers moved to PSRAM: see "USB buffers, cache
lines, one draw per pass" below for the ~29 KB that came back since.

| Memory | Used | Notes |
|---|---|---|
| SRAM (512 KB) | 442 KB (86%) | 165 KB static (code in IRAM, data/bss, ROM, cache); heap 346 KB with 276 KB in use, 71 KB free, largest block 31 KB; lowest free ever: 34 KB |
| PSRAM (8 MB) | 5041 KB (62%) | lowest free ever 1097 KB (bench buffers), largest free block 3136 KB |
| Flash (16 MB) | fully partitioned | app slots 2 x 3 MB (OTA), FAT 9.8 MB, NVS 20 KB, core dump 64 KB |
| App image | 1559 KB of 3072 KB (51%) | code 1006 KB, data 467 KB |
| FAT /ffat | 4508 KB of 9996 KB (45%) | gif.bin 4501 KB, layout.json, saved text images |
| NVS | 199 of 630 entries | 8 namespaces |

Task stacks, least ever free (bytes): main 6696, weather 4612, discord 4300,
httpd 3404, websocket 3324, flush 2820, wifi 2688, TinyUSB 2124, esp_timer 1876,
mdns 1784, tiT 1372, Tmr Svc 1328, led 1200, sys_evt 792, IDLE 688-696,
ipc 500-508.

## Internal RAM, before and after 2026-10-26

Internal RAM was the tight one: free space fell to ~3 KB within 8 s of boot,
when the Wi-Fi login and three or four TLS handshakes ran at once, and
idled near 40 KB with an 8.7 KB largest block (a TLS handshake needs ~4 KB
blocks). Four changes:

1. Wi-Fi static TX buffers 16 -> 8 (`CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM`):
   with PSRAM on they default to static, ~1.6 KB each in internal RAM for good.
2. mbedTLS: dynamic buffers, config data and CA chain freed after the
   handshake, peer certificate not kept.
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
| PSRAM | copy / write / read (1 MB buffers) | 31 / 48 / 72 MB/s (22 / 35 / 53 with 32-byte cache lines) |
| PSRAM | random read (1 MB span) | 697 ns (503 with 32-byte cache lines: each miss now fetches 64 bytes) |
| PSRAM to SRAM | copy in 4 KB pieces | 86 MB/s (58-62 with 32-byte lines) |
| Flash | raw partition read (4 KB) | 11.5 MB/s |
| Flash | memory-mapped read (cache) | 32 MB/s (26.5 with 32-byte lines) |
| Flash | FAT file read (gif.bin, 4.5 MB) | 5.7 MB/s |
| Display SPI | full 240x320 RGB444 frame (115 200 B) | 27.8-28.9 ms, 4.0-4.1 MB/s, 80-83% of the 40 MHz wire (29.6-30.6 ms with 32-byte lines) |
| USB CDC | board to PC | 0.93 MB/s sent, 0.78 MB/s received by the PC (2026-10-25) |
| USB CDC | PC to board, text lines parsed | 0.4 MB/s (43 KB in 105 ms) |
| USB | cover art, PC service to board | 115 ms on WinUSB bulk, 688 ms on HID |
| Wi-Fi | HTTP download to the PC (4 MB, 9 runs) | 0.62-0.92 MB/s |
| Wi-Fi | ping | 2-15 ms, 6 ms average |

Notes:
- A full frame takes ~28 ms per panel, so all three panels repainting at once
  cap out near 12 fps; the dirty-rect diff is what keeps normal faces at their
  frame rates. SPI runs at 40 MHz because 80 MHz gave blank panels.
- Wi-Fi throughput is low on purpose: 8 static RX and 8 static TX buffers and
  a BA window of 8 free internal RAM for TLS (see `sdkconfig.defaults`).
  Nothing on the board needs more than ~100 KB/s.
