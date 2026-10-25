# Benchmark

Carrier board (ESP32-S3 rev 0.2, 240 MHz, 16 MB flash, 8 MB octal PSRAM at
80 MHz), firmware as of 2026-10-25, PC on, Wi-Fi at -27 dBm.
Run it again with the console command `bench all` (`bench usb <KB>` for USB),
and time Wi-Fi from the PC with
`curl -o NUL -w "%{speed_download}\n" http://<board-ip>/bench?kb=4096`.

| Area | Test | Result |
|---|---|---|
| CPU | 32-bit integer multiply-add | 79 M/s (3 cycles each) |
| CPU | float multiply-add (FPU) | 79 MFLOPS |
| CPU | double multiply-add (software) | 3 MFLOPS |
| SRAM | copy / write / read | 239 / 238 / 119 MB/s |
| SRAM | random read | 13 ns |
| PSRAM | copy / write / read (1 MB buffers) | 22 / 35 / 53 MB/s |
| PSRAM | random read (1 MB span) | 504 ns |
| PSRAM to SRAM | copy in 4 KB pieces | 60 MB/s |
| Flash | raw partition read (4 KB) | 11.5 MB/s |
| Flash | memory-mapped read (cache) | 26.7 MB/s |
| Flash | FAT file read (gif.bin, 4.5 MB) | 5.2 MB/s |
| Display SPI | full 240x320 RGB444 frame (115 200 B) | 29.5-29.8 ms, 3.9 MB/s, 77-78% of the 40 MHz wire |
| USB CDC | board to PC | 0.93 MB/s sent, 0.78 MB/s received by the PC |
| Wi-Fi | HTTP download to the PC (4 MB, 3 runs) | 0.83-0.94 MB/s |
| Wi-Fi | ping | 2-8 ms, 3 ms average |

Notes:
- A full frame takes ~30 ms per panel, so all three panels repainting at once
  cap out near 11 fps; the dirty-rect diff is what keeps normal faces at their
  frame rates. SPI runs at 40 MHz because 80 MHz gave blank panels.
- Wi-Fi throughput is low on purpose: 8 static RX buffers and a BA window of 8
  free internal RAM for TLS (see `sdkconfig.defaults`). Nothing on the board
  needs more than ~100 KB/s.
- Internal RAM at the time: 24.9 KB free, largest block 7.7 KB; PSRAM 3.1 MB free.
