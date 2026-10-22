# ESP32 PC Control System

An ESP32-S3 carrier board that lives inside a PC case. It drives three 2.4"
displays, presses the motherboard's hardware-reset line, and works a 4-button
door remote, all from one ESP32-S3-N16R8 module plugged into a custom 2-layer
PCB.

## Why would I put myself through the suffering

I needed my PC online at all time, because sometime at school or work I need to remote
control the PC back at home. And as I measured, the idle time alone costs me ~40KWh (~7usd) per month.
So, I built this for 30usd instead of buying a boring and expensive IP KVM, while adding a tons more
features to fit my over-engineering stuffs hobby.

![Carrier PCB, rev 3.1: top silkscreen over F.Cu](docs/images/pcb_silkscreen_top.png)

*Carrier PCB rev 3.1, top silkscreen over the top copper layer:
45.7 x 138.2 mm, 2 layers, DRC 0/0.*

## What it does

| Function | How |
|---|---|
| 3x status displays | GMT024-08-SPI8P 2.4" ST7789 panels (240x320) on one shared SPI bus, each with its own CS, RST and PWM backlight. They plug straight into the board's right-angle sockets. |
| Motherboard reset | Optocoupler (PC817) across the motherboard's RST pins, fully isolated from the rest of the board. |
| Door remote | Decased 4-button keyfob, powered from an onboard 5V->12V boost. Four optos press its buttons. |
| PC power state | Read from the module's native USB link to the motherboard header (USB active = PC on, suspend = asleep/off). See [docs/pc-power-detection.md](docs/pc-power-detection.md). |
| Status LED | 5 mm LED below the module, for a light pipe in the enclosure. |

## Pin map (rev 3.1)

| Signal | GPIO |
|---|---|
| Display bus: MOSI / SCK / DC | 11 / 12 / 9 (MOSI and SCK through 33R) |
| Display 1: CS / RST / BL | 8 / 10 / 3 |
| Display 2: CS / RST / BL | 18 / 16 / 6 |
| Display 3: CS / RST / BL | 17 / 15 / 5 |
| Door buttons 1-4 (opto rows 1-4) | 39 / 40 / 41 / 42 |
| Motherboard reset (opto row 5) | 1 |
| Status LED | 2 |

Every opto GPIO has a 10k pull-down, so nothing gets pressed while the chip
boots or flashes. The full mapping and the reasons behind it are in
[pcb/README.md](pcb/README.md).

## Assembly notes

- **Bridge the module's `IN-OUT` solder jumper.** On ESP32-S3-N16R8 boards
  of this type, the `5VIN` pin is input-only by default: a diode stops USB
  5V from reaching it. The carrier takes all of its 5V (all three displays
  and the boost) from that pin, so with the jumper open the displays stay
  dark. Once it is bridged, never feed `5VIN` from another supply while USB
  is plugged in.
- Display panels hang **down** below the board, screen facing out, header
  edge up. That is the only way round that puts panel pin 1 (GND) on socket
  pad 1. Match the GND silk label: plugged in the wrong way, every pin is
  reversed and 5V lands on CS.
- The module rides on its sockets ~11 mm above the board. The boost, the
  five optos and most resistors sit underneath it.
- Set the boost's A+B bridges for 12V and measure the output before
  connecting the keyfob.

## Repository layout

| Path | Contents |
|---|---|
| [`pcb/`](pcb/) | atopile netlist ([`main.ato`](pcb/main.ato)), KiCad board, custom footprints, the full design notes and revision history ([pcb/README.md](pcb/README.md)) |
| [`Scripts/`](Scripts/) | Python build chain: netlist -> placement -> hand routes -> Freerouting -> chamfers -> checks -> renders, plus bench tools ([Scripts.md](Scripts/Scripts.md)) |
| [`firmware/`](firmware/) | Arduino sketches for the ESP32-S3 (below) |
| [`cad/`](cad/) | Parametric OpenSCAD enclosures ([cad/README.md](cad/README.md)) |
| [`docs/`](docs/) | Bench findings and research notes |

## Firmware

Arduino sketches, built with `arduino-cli` and the `esp32:esp32` core
(3.3.x):

| Sketch | Purpose |
|---|---|
| **`carrier_main`** | **Firmware for the assembled rev 3.1 board** (test build). Drives everything on the carrier: three panels each showing their own view, door buttons, motherboard reset, status LED, PC power detection. Details below. |
| `s3_gif_stream_test` | Single-panel bench bring-up: the PC streams a GIF as raw RGB565 ([`Scripts/gif_stream.py`](Scripts/gif_stream.py)); the sketch saves it to flash and plays it at the GIF's native ~30 fps over SPI with DMA. Also sweeps the backlight with PWM. Uses its own bench pins (BL 4, CS 5, DC 6, RST 7, SDA 15, SCL 16). |
| `s3_gif_player` / `s3_gif_player_lovyan` | GIF from RLE frames compiled into flash (Adafruit_GFX / LovyanGFX). Needs the 16MB flash + `app3M_fat9M_16MB` partition. |
| `s3_gif_wifi_player` | Diagnostic build that streams frames over WiFi. Copy `secrets.example.h` to `secrets.h` first. |
| `s3_display_fps_test` | Raw display push throughput. |
| `s3_usb_state_test` | PC power-state detection over the native USB link. |

### carrier_main

- **Displays:** three ST7789 panels, each on its own CS line and showing its
  own view: `gif`, `status` (PC state, boot-guard countdown, uptime, last
  action, door/RST state) or `stats` (bus load, per-panel fps and KB/s,
  memory, chip temperature). The default is GIF / status / stats.
- **Dirty-rectangle engine:** each panel draws into a PSRAM canvas. The
  firmware compares it with what is already on screen in 8-row bands and
  sends only the changed rectangles, so a changed number costs about 1 KB
  and a static dashboard costs nothing.
- **RGB444 on the wire** (12 bits per pixel, 25% less data than RGB565).
  Measured: the GIF panel runs at 27 fps and takes about 70% of the bus,
  while the two dashboards together take under 5%.
- **Door buttons B1-B4** (GPIO39-42): 500 ms presses, one at a time, with a
  500 ms gap enforced in firmware (the board has no hardware interlock).
- **Motherboard reset** (GPIO1): 200 ms pulse, only on the `reset` command,
  never at boot. All opto pins are driven LOW first thing at boot.
- **PC power state** from the module's own USB link to the motherboard, with
  a 30 s guard against the brief USB dropout at power-on. When the PC sleeps
  or shuts down, the backlights turn off.
- **Status LED** (GPIO2): solid = PC on, slow blink = asleep/off, short blip
  every 2 s = no USB host yet.
- **Commands**, one per line on either USB port (the module's own USB, or
  the CH343 UART at 3 Mbaud):
  `status | view <1|2|3> <gif|status|stats> | door <1-4> | reset | bl <1|2|3|all> <0-100> | led <auto|on|off>`.
- **GIF upload:** run [`Scripts/gif_stream.py`](Scripts/gif_stream.py) on
  the CH343 port. The GIF is saved to the FAT partition and replays on every
  boot.

Build + flash. The firmware needs `USBMode=default` (TinyUSB) for the USB
power events. With `CDCOnBoot=cdc`, `Serial` is the module's own USB port
and `Serial0` is the UART. `UploadMode=cdc` lets you flash through the
module's own USB port:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc,UploadMode=cdc firmware/carrier_main
arduino-cli upload  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc,UploadMode=cdc -p COM11 firmware/carrier_main
```

Build-time test switch (defaults to 0):
- `-DMOBO_RST_HOLD_TEST=1`: holds RST HIGH from boot, for testing with an
  LED on J6. **Never use it on a real motherboard**: it keeps the PC in
  reset.

### Bring-up findings (2026-10, assembled rev 3.1 board)

- **SPI runs at 40 MHz.** 80 MHz gives blank panels on the carrier's long
  shared bus, even with full drive strength on SCK/MOSI/DC. It worked on
  short bench wires. The S3's SPI clock is 80 MHz divided by a whole
  number, so 60 MHz is not possible: the driver rounds it down to 40.
- **Bus budget:** a full 240x291 frame takes 29 ms as RGB565 at 40 MHz, or
  about 21 ms as RGB444. That is enough for one full-motion panel at about
  30 fps. More full-motion panels share the bus, which is why the
  dashboards rely on dirty rectangles. The board stays as-is: no new
  routing.
- **Broadcast mode** (all CS lines held LOW, one frame sent to all three
  panels at once) plays the same GIF on all three at its native 29.7 fps.
  It is only useful when all three show identical content.
- **Panel init:** do not issue a software reset (`esp_lcd_panel_reset`
  without an RST pin). The ST7789 ignores the wake-up command for 120 ms
  after a reset, so the panel stays asleep: backlight on, no picture. The
  firmware uses the hardware RST lines instead.
- **Panel orientation:** MADCTL 0, which is rotation 2 in Adafruit's
  numbering.
- **Module 5V:** the IN-OUT jumper must be bridged (see Assembly notes).
- **Backlight:** the GMT024 panels are dim even at 100% PWM, because of the
  module's own backlight parts. Firmware can't change that.
- **Opto resistors:** the 270R opto resistors are required. Without them
  the LED still lights, but the GPIO runs at its current limit.

## Building the PCB

Toolchain: [atopile](https://atopile.io) 0.2.x, KiCad 10 (its bundled
Python provides `pcbnew`), Freerouting, and Python with pymupdf + Pillow for
the renders. The full rebuild, from `ato build` to DRC and renders, is listed
step by step in [Scripts/Scripts.md](Scripts/Scripts.md). Renders land in
`pcb/build/renders/` (not tracked); the image above is a copy.

## Status

Rev 3.1 is fabricated and assembled. The fab files come from
[`Scripts/export_gerbers.py`](Scripts/export_gerbers.py). `carrier_main`
has been tested on the board:
- all three panels work, both in broadcast and on their own CS lines;
- the motherboard RST output was checked with an LED on J6;
- PC power detection works over the module's own USB link;
- door and reset commands respond.

Still to do:
- test door buttons B1-B4 with the real keyfob connected;
- write the real dashboard content (the status and stats views are test
  placeholders);
- move the GIF's diff and RGB444 conversion to the second core, to get from
  27 fps back to the native 29.7.
