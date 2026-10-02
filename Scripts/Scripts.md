# Scripts

Both scripts run under KiCad's own bundled Python (has the `pcbnew` module; the system Python doesn't):
`"C:\Program Files\KiCad\10.0\bin\python.exe" <script>`

## make_blank_pcb.py

Creates a blank 4-layer `pcb/layouts/default/default.kicad_pcb` with the inner planes named `GND_Plane`/`Power_Planes` per the project's stackup spec. **Overwrites unconditionally** — only run once before the board exists, or deliberately to start the layout over. Never re-run after real placement/routing has begun.

## sync_netlist_to_board.py

Syncs `pcb/build/default.net` (from `ato build`) onto the real KiCad board file: loads/places every component's real footprint and assigns every pad to its correct net, using only `pcbnew`'s Python API — no GUI automation. Idempotent: re-running it keeps existing footprints' positions (only refreshes their net assignments) and grid-places any newly-added component in open space below whatever's already there. A component removed from `main.ato` is left on the board with a warning, not silently deleted. Run this after every `ato build` that changes `main.ato`'s parts or nets:

```bash
cd "E:\ESP32 PC Controller\pcb" && PYTHONIOENCODING=utf-8 ato build
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\sync_netlist_to_board.py"
```

Custom footprints live in `pcb/footprints/ESP32PCC.pretty` (lib name `ESP32PCC` in `main.ato`); the script maps that lib via `LOCAL_LIBS`, everything else loads from KiCad's stock library. Part values (270R/10k) are set from instance names via `INSTANCE_VALUES`, since atopile 0.2 drops `value` and merges identical classes' descriptions.

Exists because neither atopile's own `ato build` nor `kicad-cli` (KiCad's official CLI) nor `kicad_cli` (the third-party pierrechevalier83 GUI-automation tool) actually imports a netlist into a board file — atopile only produces the netlist text, `kicad-cli` has no import command, and `kicad_cli` only runs ERC/DRC on a project that already exists. This script is the missing piece.

## layout_board.py

Real (non-grid) component placement: positions every footprint from real measured geometry (courtyard bounding boxes read live via `pcbnew`, not assumed), fixes the resistor footprint's pad pitch to the real measured 8.128mm (stock library part is 10.16mm), handles footprint rotation for the right-angle edge-mounted connectors, places the rev-3 layout (module rotated 180° with USB-C at the top edge, boost/optos/resistors under the module, the two THT 33R bus resistors in the top-right corner, displays at 47 mm pitch down the east edge), draws the L-shaped board outline (west RA sockets flush +0.4mm, display sockets overhanging the east edge 1mm with their silk clipped to the board, 0.3mm courtyard margin elsewhere, east strip for displays 2/3) and 4 mounting holes at fixed spots (collision-checked against courtyards, the module body and the L-shape), trims the resistor silkscreen to the shortened pitch, swaps the resistor 3D model to the stock P7.62mm STEP stretched in X to 8.128mm (a shifted P10.16 model left both leads outside their holes), adds per-pin silkscreen labels on every connector, and the assembly markings (module ANTENNA/USB-C ends + 3V3/GND pin 1s, resistor values inside the bodies, PC817 pin-1 dot replacing the notch). Never `fp.Remove()` footprint graphics here — it corrupts later footprints' SWIG wrappers; move items to another layer instead. Run after `sync_netlist_to_board.py`.

**Not idempotent in place** — re-running it on its own output corrupts other live `pcbnew` SWIG object references (same class of bug as the `NETINFO_ITEM` removal issue below). Always rebuild from a fresh blank+synced board first:

```bash
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\make_blank_pcb.py"
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\sync_netlist_to_board.py"
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\layout_board.py"
```

Full chain order: `make_blank_pcb` → `sync_netlist_to_board` → `layout_board` → `add_pours` → `route_spi` → `route_board` → `chamfer_corners` → `check_spi_topology` + `kicad-cli pcb drc` → `render_board`.

## add_pours.py

Pours the two plane layers: GND on `In1` (net `gnd`) and 5V on `In2` (net `v5`) — both named explicitly in `main.ato`. Also adds the antenna keepout rule area (all copper layers, no tracks/vias/fill) under the module's PCB antenna; it must exist before `route_board.py` so Freerouting routes around it. Since every component on this board is through-hole, THT pad copper already exists on every layer by default, so these two pours alone fully connect every GND/5V pad with zero explicit traces or vias. Run after `layout_board.py`.

## make_fit_models.py

Writes simple VRML fit-check models into `pcb/footprints/ESP32PCC.3dshapes/`: the BS01 boost on its 4 single male pins (referenced by the `Boost_CKCS_BS01` footprint) and an envelope of the plugged-in ESP32 DevKit at its real height on the sockets (attached to U2 by `layout_board.py`), so renders show what has to fit under the module. Prints the stack heights and clearance. Re-run after changing `BOOST_H`.

```bash
python "E:\ESP32 PC Controller\Scripts\make_fit_models.py"
```

## route_spi.py

Hand-routes the display nets (SCK, MOSI, DC, RST, CS1-3, plus the two module-to-series-resistor stubs) on F.Cu as **locked** tracks: shared lines as a true daisy chain MCU -> display1 -> display2 -> display3 (each segment ends on a pad, no T-junctions, no vias), CS lines point-to-point. Routes are computed from placement constants (`XR`, `XD`, `PITCH`, `C1`, `R33_X`) that must match `layout_board.py` -- change both together. Freerouting would build trees with mid-trace T-joins and drop most of the bus onto B.Cu. Run after `add_pours.py`, before `route_board.py`.

```bash
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\route_spi.py"
```

## chamfer_corners.py

Removes every 90-degree (and sharper) track corner on the board, hand-routed bus and Freerouting tracks alike: both legs are cut back 0.5mm and joined by a 45-degree diagonal. Each diagonal is collision-checked against other-net pads/tracks/vias (pcbnew shape `Collide`, 0.21mm) and halved down to 0.12mm if needed; corners on a pad/via are skipped. Freerouting's nm-long segments at bends are reused as the diagonal rather than deleted (`board.Remove()` corrupts the other SWIG wrappers, and a second `LoadBoard` in the same process fails too). Run after `route_board.py`.

```bash
"C:\Program Files\KiCad\10.0\bin\python.exe" "E:\ESP32 PC Controller\Scripts\chamfer_corners.py"
```

## check_spi_topology.py

Verifies the display bus after routing: F.Cu only, 0 vias, no node with 3+ track ends (T/star), and the chain order MCU -> D1 -> D2 -> D3 per shared net. Run after `route_board.py` or any manual edit to those nets.

## route_board.py

Auto-routes the remaining (low-speed) signal nets around the locked display bus (max 60 passes) with [Freerouting](https://github.com/freerouting/freerouting) (external tool — neither `kicad-cli` nor `pcbnew` include an autorouter; KiCad dropped its own years ago). Cached at `K:\cache\freerouting\freerouting-2.4.1.jar`, not committed to this repo. Pipeline: export Specctra DSN (`pcbnew.ExportSpecctraDSN`) → run Freerouting headless, restricted to `F.Cu`/`B.Cu` (the plane layers are marked non-routable) → import the routed SES back (`pcbnew.ImportSpecctraSES`) → **re-fill the copper zones** (zones filled before routing don't know about the vias routing just added, which DRC catches as a zone/via clearance violation until re-filled).

**Only run against a fresh, unrouted board** (straight out of `add_pours.py`) — re-running it on a board it already routed left a net unrouted in practice (Freerouting tried to reuse existing traces as fixed obstacles and got stuck), where routing fresh hit 0 unrouted both times tried. To reroute after a layout change, rebuild the whole chain from `make_blank_pcb.py` again rather than routing on top of existing traces.

## render_board.py

Refreshes `pcb/build/renders/`: one PNG per copper layer (F.Cu, In1 GND, In2 5V, B.Cu), top silkscreen over F.Cu, and the slanted 3D overview (also copied to `pcb/build/board_preview.png`) via `kicad-cli` SVG export + pymupdf. Runs under the system Python (needs pymupdf + Pillow); intermediates go to `K:\scratch\pdf`. Run after the full routing chain + DRC pass.

```bash
python "E:\ESP32 PC Controller\Scripts\render_board.py"
```
