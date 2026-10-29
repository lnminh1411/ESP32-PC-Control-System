# Hardware

The carrier board, rev 3.1 (L-shaped, fabricated and assembled). The netlist,
layout and design notes are in [pcb/README.md](../pcb/README.md); the case is
in [enclosure/README.md](../enclosure/README.md).

![Carrier PCB, rev 3.1: top silkscreen over F.Cu](images/pcb_silkscreen_top.png)

- 45.7 x 138.2 mm, 2 layers, DRC 0/0
- ESP32-S3-N16R8 module on sockets, three ST7789 panels on one SPI bus
- Optocouplers (PC817) for the motherboard reset and the four keyfob buttons
- A 5 V to 12 V boost module for the keyfob
- Native USB to a motherboard header: PC power state, stats link and USB
  power commands in one cable

## What it does

| Function | How |
|---|---|
| 3x status displays | GMT024-08-SPI8P 2.4" ST7789 panels (240x320) on one shared SPI bus, each with its own CS, RST and PWM backlight. They plug straight into the board's right-angle sockets. |
| Motherboard reset | Optocoupler (PC817) across the motherboard's RST pins, fully isolated from the rest of the board. |
| Door remote | Decased 4-button keyfob, powered from an onboard 5V->12V boost. Four optos press its buttons. |
| PC power state | Read from the module's native USB link to the motherboard header (USB active = PC on, suspend = asleep/off). See [pc-power-detection.md](pc-power-detection.md). |
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
boots. (That turned out not to hold in ROM download mode; see
[the eFuse note](#keyfob-presses-during-flashing-efuse-fix).) The netlist
([pcb/main.ato](../pcb/main.ato)) is the source of truth;
`python scripts/check_pins.py` checks it against
[firmware/main/config.h](../firmware/main/config.h).

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
- Unplug the motherboard RST cable before flashing any other test firmware
  that toggles GPIOs.
- On a new module, burn the `DIS_USB_OTG_DOWNLOAD_MODE` eFuse before the
  keyfob is connected (below).

### Keyfob presses during flashing (eFuse fix)

With the keyfob connected, its LED lit every time the board was flashed:
door buttons B1/B2 were being pressed.

Cause: in ROM download mode the ESP32-S3 ROM sets up USB-OTG download and
drives GPIO38/39/40 as outputs for an external USB PHY (VMO / VPO / OEN).
GPIO39 and GPIO40 are door rows 1 and 2, and a driven output beats the 10k
pull-downs. The application firmware never runs in download mode, so it
cannot stop this.

Fix: burn the one-time eFuse `DIS_USB_OTG_DOWNLOAD_MODE`:

```bash
espefuse.py -p <ROM port> burn_efuse DIS_USB_OTG_DOWNLOAD_MODE 1
```

After this, download mode leaves GPIO32 and up as inputs and the door rows
stay low. eFuses can only be written once, so this cannot be undone. What
still works afterwards:
- flashing over the USB-Serial-JTAG port (a 1200-baud touch on the app's COM
  port reboots into the ROM, which shows up as a new COM port;
  `scripts/idf_flash.py` does this);
- the application's own USB-OTG device (CDC console, a WinUSB bulk stats
  link, and vendor HID as the stats link's fallback and for the power button).

Only USB-OTG *download* is gone, which this board never used. A later PCB
revision could move the door rows off GPIO38-42 instead.

## Bring-up findings (2026-10, assembled rev 3.1 board)

- **SPI runs at 40 MHz.** 80 MHz gives blank panels on the carrier's long
  shared bus, even with full drive strength on SCK/MOSI/DC. The S3's SPI
  clock is 80 MHz divided by a whole number, so 60 MHz is not possible: the
  driver rounds it down to 40.
- **Bus budget:** a full 240x291 frame takes 29 ms as RGB565 at 40 MHz, or
  about 21 ms as RGB444. That is enough for one full-motion panel at about
  30 fps. More full-motion panels share the bus, which is why the faces
  rely on dirty rectangles.
- **Broadcast mode** (all CS lines held LOW, one frame sent to all three
  panels at once) plays the same GIF on all three at its native 29.7 fps.
  It is only useful when all three show identical content.
- **Panel init:** do not issue a software reset (`esp_lcd_panel_reset`
  without an RST pin). The ST7789 ignores the wake-up command for 120 ms
  after a reset, so the panel stays asleep: backlight on, no picture. The
  firmware uses the hardware RST lines instead.
- **Backlight:** the GMT024 panels are dim even at 100% PWM, because of the
  module's own backlight parts. Firmware can't change that.
- **Opto resistors:** the 270R opto resistors are required. Without them
  the LED still lights, but the GPIO runs at its current limit.
- **ROM download mode drives GPIO38-40**, which pressed the keyfob during
  flashing. Fixed with an eFuse, see
  [above](#keyfob-presses-during-flashing-efuse-fix).
- **Main task stack:** 8 KB overflowed while drawing the 128 px clock digits
  and rebooted the board. The ESP-IDF build gives the main task 10 KB
  (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`).
- **PSRAM at 40 MHz with ECC** since 2026-10-28: at 80 MHz the PC's power-on
  sag flipped PSRAM bits ([firmware.md](firmware.md#psram-hardening)).

## Building the PCB

Toolchain: [atopile](https://atopile.io) 0.2.x, KiCad 10 (its bundled
Python provides `pcbnew`), Freerouting, and Python with pymupdf + Pillow for
the renders. The full rebuild, from `ato build` to DRC, renders and Gerbers,
is listed step by step in [scripts/README.md](../scripts/README.md). Renders
land in `pcb/build/renders/` (not tracked); the image above is a copy.
