# ESP32 PC Control System

An ESP32-S3 carrier board that lives inside a PC case. It drives three 2.4"
displays, presses the motherboard's hardware-reset line, and works a 4-button
door remote, all from one ESP32-S3-N16R8 module plugged into a custom 2-layer
PCB.

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
| `s3_gif_stream_test` | Display bring-up: the PC streams a GIF as raw RGB565 ([`Scripts/gif_stream.py`](Scripts/gif_stream.py)); the sketch saves it to flash and plays it at the GIF's native ~30 fps over SPI with DMA. Also sweeps the backlight with PWM. Uses its own bench pins. |
| `s3_gif_player` / `s3_gif_player_lovyan` | GIF from RLE frames compiled into flash (Adafruit_GFX / LovyanGFX). Needs the 16MB flash + `app3M_fat9M_16MB` partition. |
| `s3_gif_wifi_player` | Diagnostic build that streams frames over WiFi. Copy `secrets.example.h` to `secrets.h` first. |
| `s3_display_fps_test` | Raw display push throughput. |
| `s3_usb_state_test` | PC power-state detection over the native USB link. |

Typical build + flash (ESP32-S3 with 16MB flash and 8MB octal PSRAM):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB firmware/s3_gif_stream_test
arduino-cli upload  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB -p COM15 firmware/s3_gif_stream_test
```

## Building the PCB

Toolchain: [atopile](https://atopile.io) 0.2.x, KiCad 10 (its bundled
Python provides `pcbnew`), Freerouting, and Python with pymupdf + Pillow for
the renders. The full rebuild, from `ato build` to DRC and renders, is listed
step by step in [Scripts/Scripts.md](Scripts/Scripts.md). Renders land in
`pcb/build/renders/` (not tracked); the image above is a copy.

## Status

Rev 3.1 board layout is complete (DRC clean, SPI topology checked) and not
yet fabricated. The firmware so far is bench bring-up: the displays, the GIF
pipeline and PC power detection are proven on hardware; the opto/keyfob and
reset control firmware is still to be written.
