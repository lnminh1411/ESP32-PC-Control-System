# Carrier PCB (atopile)

Single-file atopile project: [`main.ato`](main.ato). Deliberately has zero external imports (no stdlib `Resistor`/`LED` etc.) — every part is defined locally with explicit pins + footprint, so it doesn't depend on guessing the right stdlib import path for whatever atopile version you have installed.

**2026-09-30: rewritten for a single-MCU architecture**, replacing the earlier dual-MCU (SuperMini + DevKit V1, cross-ping watchdog) design. One ESP32-S3-N16R8 DevKitC-1-style module (44-pin/2x22, 16MB flash/8MB PSRAM, dual USB-C) now drives all three displays, the full 5-row optocoupler bank, and the motherboard RST line. Netlist reverse-engineered from `../diagrams/circuit_image.png` via pixel-level exact-color wire tracing (this tool has a track record of color-collision bugs in its exports, so every net was confirmed at both endpoints, not eyeballed) and cross-checked against the module's own silkscreen for the 44-pin layout.

**Rebuilt clean through `ato build`, and a real `.kicad_pcb` now exists and is populated** — `pcb/layouts/default/default.kicad_pcb` has all 19 real components placed (grid layout, not final) with every net correctly wired to real footprint pads. Verified via `kicad-cli pcb drc`: 0 real errors (the only flagged item is the already-known missing board outline; the 73 "unconnected" items are just unrouted ratsnest, expected before real routing) and a `kicad-cli pcb render` visual check.

Getting from netlist to a populated board took one non-obvious fix worth knowing about if this ever needs redoing: every `component`'s `pin` declarations in `main.ato` originally used descriptive names (`pin gpio4`, `pin p1`, ...), matching the old dual-MCU file's own convention. That's wrong for stock KiCad footprints — their pads are numbered (`"1"`, `"2"`, ...), not named, so a descriptively-named pin never actually binds to a real physical pad; it only ever looked fine because `ato build`'s own netlist target doesn't touch real footprints, so nothing caught it. Confirmed by loading each footprint via `pcbnew`'s Python API and reading real pad `(x, y)` positions — the 44-pin socket in particular numbers its pads in a **zigzag** (pad 1/3/5/.../43 down one physical column, pad 2/4/6/.../44 down the other), not straight down each column as declared. Fixed everywhere in `main.ato` via `signal <name> ~ pin <N>`, netting the friendly name to the real numbered pad. See the block comment above `Resistor270` in `main.ato` for the full explanation.

One gotcha if you regenerate `ato.yaml` from scratch: the version key is **`ato-version`** (hyphenated), not `ato_version` — the underscore form fails with an opaque `member 'ato_version' of 'ProjectConfig' objects is not valid SemVer string` error that has nothing to do with the actual version string.

## Build

```bash
cd pcb
PYTHONIOENCODING=utf-8 ato build
```
(`PYTHONIOENCODING=utf-8` works around a Windows-console-only crash in atopile's `designator-map` step — an unrelated `rich`/`colorama` Unicode bug, not a project issue.)

Neither `ato build`, `kicad-cli` (KiCad's own official CLI), nor the third-party `kicad_cli` (pierrechevalier83, GUI-automation over `eeschema`/`pcbnew`, Linux/X11/Docker-only, ERC+DRC only) actually imports a netlist into a board file. The missing piece is [`../Scripts/sync_netlist_to_board.py`](../Scripts/sync_netlist_to_board.py), built directly on KiCad's bundled `pcbnew` Python API — no GUI automation. Run after every `ato build` that changes parts or nets:

```bash
"C:\Program Files\KiCad\10.0\bin\python.exe" "..\Scripts\sync_netlist_to_board.py"
```

It's idempotent — re-running keeps existing footprints' placement and only refreshes net assignments; a newly-added part gets grid-placed in open space, a removed one is left with a warning rather than silently deleted. See [`../Scripts/Scripts.md`](../Scripts/Scripts.md).

**Board (rev 3)**: L-shaped, 66.6 x 122.2 mm overall, 1.6 mm thick, 4-layer, placed + autorouted (DRC 0/0). Main body 66.6 x 59.8 mm; a 13.6 mm strip runs down the east edge to carry displays 2/3. The ESP32 module is mounted **rotated 180°**: USB-C end overhangs the **top** edge (the enclosure's plug-in side), PCB antenna overhangs the **bottom** edge with a no-copper keepout between the rows. Boost, the 5 optos and the 7 axial resistors sit **under the module** (it rides ~11 mm up on its sockets; boost ~7.5 mm tall). Displays plug in **directly** (no cables): sockets at 47 mm pitch (37 mm panel + 10 mm gap), overhanging the east edge 1 mm so the panel's header clears; panel 1's top is level with the board top, panel 3 extends ~9 mm past the strip's bottom. Mounting holes: 2 in the west strip, 2 in the east strip between displays. Renders of each copper layer, the silkscreen and a 3D overview are in `build/renders/`.

**Net names** are declared explicitly in `main.ato` (`gnd`, `v5`, `v12`, `disp_*`, `mb_rst`, `mb_rst_gnd`, `led_a`); without them atopile names each net after a random member pin (GND came out as `cathode`, 12V as `boost-collector`).

**Antenna keepout**: `add_pours.py` adds a no-copper rule area (all 4 layers: no tracks, vias or plane fill) under the plugged-in module's PCB antenna (between the socket rows, board top edge to Y=7mm), so the GND/5V planes don't detune WiFi/BLE.

**Silkscreen**: every off-board connector has per-pin labels (displays, keyfob 12V/B1-B4/GND, RST pair, spare header GPIO numbers) for dry-fitting. Assembly markings:
- **ESP32-S3 module**: "ANTENNA END" at the top edge, "USB-C END" at the bottom; pin 1 of each row labelled (left row 3V3, right row GND).
- **Resistors**: value printed inside each body (`270R` x6, `10k` for the GPIO39 pull-down) instead of the reference.
- **PC817 optos**: pin 1 (LED anode) marked with a **dot** in the pin-1 corner, matching the fitted part's moulded dot (the library's top-middle notch is removed). Pin 1 is also the square pad. Place dot-to-dot.

**Display bus routing**: SCK/MOSI/DC/RST are hand-routed ([`../Scripts/route_spi.py`](../Scripts/route_spi.py)) as a **daisy chain on L1 (F.Cu) only** -- MCU -> display1 -> display2 -> display3, each segment pad-to-pad, no T-junction/star stubs, no vias, whole run over the unbroken In1 GND plane. SCK and MOSI each pass a **33R THT series resistor** (source termination; fit a wire link to bypass), standing side by side in the board's top-right corner above display 1: module pin -> short F.Cu stub north -> resistor -> straight down the outer right lanes. CS1-3 are point-to-point, also F.Cu-only. Freerouting only handles the low-speed nets around them. **No 90-degree track corners** anywhere on the board (rev 3.2): every corner is a 45-degree chamfer ([`../Scripts/chamfer_corners.py`](../Scripts/chamfer_corners.py)). Verified by [`../Scripts/check_spi_topology.py`](../Scripts/check_spi_topology.py). Chain lengths: SCK 132 mm (+14 mm module-to-resistor), MOSI 124 mm (+14 mm), DC 137 mm, RST 140 mm (the 2 x 47 mm display pitch alone is 94 mm of that).

**Stackup**: 4-layer, per the user's spec — L1 (Top) = high-speed signal (SCL/SDA/CS/ST7789 control lines + component pads), L2 (Inner1) = solid unbroken GND plane (no traces), L3 (Inner2) = power planes/fills (split 5V zone, 3.3V zone, 12V boost run), L4 (Bottom) = low-speed signal (optocoupler traces, status LED, motherboard RST traces). [`../Scripts/make_blank_pcb.py`](../Scripts/make_blank_pcb.py) already set the board to 4 copper layers with the inner planes named `GND_Plane`/`Power_Planes`; zone fills/pours still need to be drawn in KiCad once there's a board outline to fill within.

## What's on it

| Connector | Pins | Footprint |
|---|---|---|
| N16R8 module socket | 44 (2x22) | female, ESP32-S3-N16R8 module plugs in with its own male header pins |
| Boost converter | 4 | CKCS BS01 5V→12V (4R7 coil rev), soldered face up via 4 male pins — custom footprint `footprints/ESP32PCC.pretty/Boost_CKCS_BS01` (pads measured off ruler photos, ±0.15mm: IN−/IN+ 3.90mm, OUT+/OUT− 7.90mm, IN↔OUT 17.75mm). **Print the footprint 1:1 and dry-fit the module before ordering.** Set A+B bridges for 12V and meter it before connecting the fob. **Mount on 4 single male pins** (pads aren't on a 2.54mm grid — cut singles off a strip): long end down through this board (trim underneath), short end up through the module. Sits **under the ESP32**: 2.5mm header plastic + ~4mm module = top at ~6.5mm, pin tips 5.5mm, DevKit underside at 11.0mm (8.5mm socket + 2.5mm DevKit header plastic) → **~4.5mm clearance** (`build/renders/8_side_stack.png`). |
| Keyfob connector | 1x6 | female right-angle: 1=12V, 2-5=BTN1-4 (opto 1-4 emitters), 6=GND |
| Display sockets | 3x 7 (1x7) | female right-angle at 47 mm pitch, one per GMT020-02-7P ST7789 panel, panel plugs straight in. Shared SPI bus + individual CS. Panel plugs in **display side up** — back side up reverses the pin order (panel DC onto 5V). Pins labelled GND/5V/SCL/SDA/RST/DC/CSn on silkscreen |
| PC817 bank | 5x DIP-4 | real `Package_DIP:DIP-4_W7.62mm`, direct-solder |
| Status LED | 1x, 5mm THT | pad 1 (square) = cathode |
| Resistors | 6x 270Ω + 5x 10kΩ + 2x 33Ω, all THT | THT axial (DIN0207, leads bent to 8.128 mm) — 5 for opto LED drive, 1 for the status LED, 5x 10k pull-downs on every opto GPIO (39 reset, 1/42/41/40 door buttons), 2x 33R SCK/MOSI series termination (top-right corner) |
| Motherboard RST pair | 1x2 | female right-angle: 1 = opto 5 collector (to RST+), 2 = emitter (to RST GND) |
| Spare header | 1x16 | male: GPIO 4,5,6,7,15,16,3,46,13,14,2,38,0,45,48,47 (not GPIO35/36/37 octal PSRAM, GPIO19/20 USB D−/D+) |

USB-OTG (motherboard front-panel USB passthrough) is **not represented on this board** — it's the module's own onboard second USB-C port, wired externally via a Type-E-to-Type-C cable straight to the motherboard's front-panel USB header. No carrier-PCB copper needed for it.

## Confirmed pin mapping

- Display bus (shared): SDA/MOSI=GPIO11, SCL/SCK=GPIO12 (FSPI IO_MUX pins, 80 MHz-capable; each via 33R), RST=GPIO10, DC=GPIO9. Per-display CS: CS1=GPIO8, CS2=GPIO18, CS3=GPIO17. **Changed 2026-10-02** (was RST=6, DC=7, CS=17/16/15) so the whole bus sits on adjacent module pins facing the displays -- update firmware pin defines.
- Display VCC → module's 5Vin pin (not 3V3) — **intentional**, not a bug: the final display part has its own onboard 5V→3.3V regulator, which avoids loading the module's single onboard 3.3V LDO across all 3 panels simultaneously.
- Optocoupler bank: GPIO1/42/41/40/39 → opto rows 1–5. Rows 1–4 are door-remote buttons (power-injection: boost 12V+ bussed to rows 1–4 collectors only, emitters out to the keyfob connector). Row 5 is the motherboard hardware-reset line — its collector/emitter go **only** to the RST pair, never the 12V bus. **Every opto GPIO has a 10kΩ pull-down** (rev 3.1: GPIO1/42/41/40 added next to GPIO39's): GPIO39-42 are JTAG pins that can come up with a ~45k pull-up during reset/boot/flashing, and ~50µA through an opto is enough for the fob's CMOS button input to see a phantom door press. With 10k the pin sits at ~0.6V, below the LED's ~1.1V turn-on.
- Status LED: GPIO21 → 270Ω → LED → GND.

### Firmware: opto GPIO init (GPIO39 reset + GPIO1/42/41/40 doors)

Same pattern for all five opto pins (`gpio_set_level(pin, 0)` then `pinMode(pin, OUTPUT)`, never `INPUT_PULLUP`; doors also keep the no-simultaneous-UP/DOWN rule in firmware). Pre-stage the output latch LOW **before** switching the pin to output, so the pin goes Hi-Z (held low by the 10k) → driven LOW, with no window where it can drive HIGH:

```cpp
#include "driver/gpio.h"
const int PIN_MOBO_RST = 39;

void setup() {
  // 1. Pre-stage the output latch to LOW while the pin is still Hi-Z.
  //    gpio_set_level, NOT digitalWrite: on arduino-esp32 3.x (3.3.12
  //    installed) digitalWrite on a pin not yet claimed by pinMode is a
  //    no-op that only logs "IO 39 is not set as GPIO" -- the latch is
  //    never written. gpio_set_level writes the register in any mode.
  gpio_set_level((gpio_num_t)PIN_MOBO_RST, 0);

  // 2. Safely switch the pin mode from input to output.
  pinMode(PIN_MOBO_RST, OUTPUT);

  // Clean transition: Hi-Z (pulled down by 10k) -> driven LOW (0V).
}
```

Rules:
- Reset = short pulse only (HIGH ~200 ms, then LOW). Holding HIGH keeps the PC in reset; it never powers it off.
- Never trigger a reset from `setup()`/boot: if the ESP is powered from the PC's USB and the board drops 5V during reset, that's an endless reset loop.
- Never use `INPUT_PULLUP` on GPIO39: the ~45k internal pull-up against the 10k pull-down lands near the opto LED's turn-on voltage.
- Unplug the RST cable before running any GPIO-toggling test firmware (e.g. the mculab test suite).

No hardware interlock exists on this board for the door UP/DOWN optocouplers — that 500ms/no-simultaneous rule from the original brief has to live in firmware, same as before.

## Superseded

The old dual-MCU (SuperMini + DevKit V1) design, its cross-ping watchdog note, the SuperMini power-pin ambiguity, and the GPIO7 display-SCK conflict are all moot under the new single-MCU architecture — that module has its own clean, fully-populated 44-pin header with no ambiguous silkscreen labels. See git history for the old `main.ato`/README if that context is ever needed again.
