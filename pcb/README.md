# Carrier PCB (atopile + KiCad), rev 3.1

Single-file atopile project: [`main.ato`](main.ato), the source of truth
for the netlist. It has no external imports: every part is defined locally
with explicit pins and footprint. The KiCad board is
`layouts/default/default.kicad_pcb`, generated and routed by the scripts in
[`../scripts/`](../scripts/README.md).

## Build

```bash
cd pcb
PYTHONIOENCODING=utf-8 ato build
```

`PYTHONIOENCODING=utf-8` works around a Windows-console crash in atopile's
`designator-map` step (a `rich`/`colorama` Unicode bug). `ato build` only
writes the netlist; [`../scripts/sync_netlist_to_board.py`](../scripts/sync_netlist_to_board.py)
puts it onto the board. The full chain is in
[`../scripts/README.md`](../scripts/README.md).

If you regenerate `ato.yaml`, the version key is **`ato-version`**
(hyphenated). The underscore form fails with an unrelated-looking
"not valid SemVer string" error.

## Board

- **L-shaped, 45.7 x 138.2 mm**, 1.6 mm thick, **2 layers**, DRC 0/0.
  Main body 45.7 x 88.0 mm, plus a 16.7 mm strip down the east edge that
  carries displays 2 and 3.
- The ESP32 module is mounted **rotated 180°**: the USB-C end overhangs the
  **top** edge (the enclosure's plug-in side), the PCB antenna is at the
  bottom of the module with a no-copper keepout under it (all layers,
  Y = 54.5-67.5 mm). The main body continues below the antenna to hold the
  RST pair, keyfob socket and status LED, so expect slightly less WiFi/BLE
  range than with the antenna over air.
- The boost, the 5 optos and 11 of the axial resistors sit **under the
  module**, which rides ~11 mm up on its sockets (boost top ~6.5 mm, ~4.5 mm
  clearance).
- **Displays plug in directly**: GMT024-08-SPI8P 2.4" panels (PCB 46.9 x
  71 mm, 8-pin header) in right-angle sockets at **51.9 mm pitch**,
  overhanging the east edge 1 mm so the panel header clears. Pin order
  GND VCC SCL SDA RST DC CS BL = socket pads 1-8. **Panels hang down below
  the board**, screen facing out, header edge up: the only way round that
  puts panel pin 1 (GND) on pad 1. Plugged in standing up, all 8 pins
  reverse (5V onto CS).
- Mounting holes (M2.5): top-right, bottom-left of the main body, between
  displays 1/2 and between displays 2/3.

### Stackup and routing

- **F.Cu carries all routing; B.Cu is one unbroken GND pour** except 5
  short B.Cu jumpers (27 mm total, 6 vias) where a line must cross the
  display bus or the opto anode bus. No B.Cu jumper crosses under SCK or
  MOSI. Every part is through-hole, so the pour connects every GND pad.
- **5V is a routed 0.4-0.5 mm track** (net class `Power`: 0.5 mm track,
  0.8/0.4 mm via, covers 5V and 12V) from J1 pin 21 down the west side of
  J1 to a trunk under the display sockets. 12V (boost -> opto collectors,
  keyfob) is a 0.5 mm bus, 0.4 mm to the keyfob. Nothing uses 3V3.
- **Display bus**: SCK, MOSI and DC are a true daisy chain on F.Cu (MCU ->
  display 1 -> 2 -> 3, every segment pad to pad, no T-junctions, no vias).
  SCK and MOSI each pass a **33R THT series resistor** (source termination;
  fit a wire link to bypass) lying flat in the top-right corner. CS1-3 and
  RST1-3 are point-to-point. Chain lengths: SCK 144 mm, MOSI 146 mm, DC
  147 mm. Verified by [`../scripts/check_spi_topology.py`](../scripts/check_spi_topology.py).
- **Everything is hand-routed** by [`../scripts/route_spi.py`](../scripts/route_spi.py);
  Freerouting stays only as a safety net. Every track corner is a 45°
  chamfer.
- **SI note**: on 2 layers the SPI return path is the B.Cu pour 1.6 mm
  below, and three jumper slots (BL2, BL3, RST2, each <= 6 mm) cross under
  the bus lanes in the strip. The bus runs at 40 MHz (see the root README);
  if a panel ever shows noise, lower the SPI clock first.

**Net names** are declared explicitly in `main.ato` (`gnd`, `v5`, `v12`,
`disp_*`, `mb_rst`, `mb_rst_gnd`, `led_a`). Without them atopile names each
net after a random member pin.

**Board references come from the atopile instance names** (J1 `mcu.left`,
J2 `mcu.right`, J3-J5 displays, J6 RST pair, J7 keyfob, PS1 boost, U1-U5
`opto1-5`, R1-R13, D1 LED), not atopile's declaration-order numbers, so
adding or removing a part does not renumber the rest.

### Silkscreen

- Every off-board connector has per-pin labels (displays
  GND/5V/SCL/SDA/RSTn/DC/CSn/BLn, keyfob 12V/B1-B4/GND, RST pair).
- **Module**: "USB-C END (TOP EDGE)" at the top edge, "ANTENNA END" at the
  bottom; pin 1 of each row labelled (left row J2 = GND, right row J1 = 3V3).
- **Resistors**: the value is printed inside each body (`270R` x6, `10k`
  x5, `33R` x2).
- **PC817 optos**: pin 1 (LED anode) is the square pad and has a **dot**,
  matching the part's moulded dot. Place dot to dot.

## Parts

| Connector / part | Pins | Footprint / notes |
|---|---|---|
| N16R8 module socket (J1, J2) | 44 (2x22) | Female; the module plugs in on its own male headers. Socket row spacing is 25.4 mm, measured off the real module (not Espressif's 22.86 mm reference). |
| Boost converter (PS1) | 4 | CKCS BS01 5V->12V, custom footprint `footprints/ESP32PCC.pretty/Boost_CKCS_BS01` (pads measured ±0.15 mm). Mount on 4 single male pins: long end down through the board, short end up through the module. Set the A+B bridges for 12V and meter it before connecting the keyfob. |
| Keyfob connector (J7) | 1x6 | Female right-angle, bottom edge of the main body: 1 = 12V, 2-5 = B1-B4 (opto 1-4 emitters), 6 = GND |
| Display sockets (J3-J5) | 3x 1x8 | Female right-angle, see Board above |
| PC817 bank (U1-U5) | 5x DIP-4 | `Package_DIP:DIP-4_W7.62mm`, turned 180°, outputs facing the west edge |
| Status LED (D1) | 5 mm THT | Pad 1 (square) = cathode; below the module beside J6, for a light pipe |
| Resistors (R1-R13) | 6x 270R + 5x 10k + 2x 33R | Axial DIN0207 lying flat, leads bent to the measured 8.128 mm pitch: 5 opto LED drive, 1 status LED, 5 opto-GPIO pull-downs, 2 SCK/MOSI series terminations |
| Motherboard RST pair (J6) | 1x2 | Female right-angle, west edge below the module: 1 = opto 5 collector (to RST+), 2 = emitter (to RST GND) |

The PC link (USB-OTG, the module's second USB-C port) is a Type-E-to-Type-C
cable straight to the motherboard's internal USB header. No carrier copper.

## Pin mapping

The table is in the [root README](../README.md#pin-map-rev-31). Design
reasons behind it:

- Display bus on FSPI IO_MUX pins (MOSI 11, SCK 12). Per-display RST lines
  so a hung panel can be reset alone. BL1 is GPIO3 (pin 13, below the bus):
  pin 19 sits inside the loop the SCK/MOSI risers make, and a jumper there
  would cut the GND pour under both fast lines. GPIO3 is a strap pin only
  when the JTAG-select eFuse is burnt (it isn't by default).
- Opto drive GPIOs (B1-B4 = 39/40/41/42, reset = 1) follow J2's pin order so
  the drive lines reach the optos without crossing. GPIO1 for the reset
  line has no boot-time pull-up glitch.
- **Every opto GPIO has a 10k pull-down.** GPIO39-42 are JTAG pins that can
  come up with a ~45k pull-up during reset/boot/flashing, and ~50 µA through
  an opto is enough for the fob's CMOS button input to see a phantom press.
  With 10k the pin sits at ~0.6 V, below the LED's ~1.1 V turn-on.
- Display VCC is the module's 5Vin, not 3V3, on purpose: the panels have
  their own 3.3V regulator. **Bridge the module's `IN-OUT` jumper**: by
  default `5VIN` is input-only and the carrier gets no 5V.
- Unused module GPIOs: 0, 4, 7, 13, 14, 21, 38, 45, 46, 47, 48 (48 = onboard
  WS2812).

### Opto GPIO rules (firmware)

All five opto pins are initialised the same way, implemented in
[`../firmware/main/optos.cpp`](../firmware/main/optos.cpp):
pre-stage the output latch LOW with `gpio_set_level(pin, 0)` while the pin
is still Hi-Z, then `pinMode(pin, OUTPUT)`. The pin goes Hi-Z (held low by
the 10k) -> driven LOW, with no window where it can drive HIGH. Use
`gpio_set_level`, not `digitalWrite`: on arduino-esp32 3.x, `digitalWrite`
on a pin not yet claimed by `pinMode` is a no-op.

- Reset is a short pulse only (HIGH 200 ms, then LOW). Holding it HIGH keeps
  the PC in reset; it never powers it off.
- Never pulse reset from `setup()`: the ESP is powered from the PC's USB, so
  a reset at boot could loop forever.
- Never use `INPUT_PULLUP` on an opto GPIO: the ~45k internal pull-up
  against the 10k pull-down lands near the opto LED's turn-on voltage.
- The board has no hardware interlock between door buttons; the
  one-at-a-time rule and the 500 ms gap live in firmware.
