# Enclosure (rev 3.1 board)

Four printed parts, parametric CadQuery: plinth block, lid, screen mask, screen backplate. The LED stays visible. The earlier full case is kept in
[`archive/`](archive/).

```bash
pip install cadquery
python enclosure/dump_pads.py         # KiCad Python: through-hole pad positions -> pads.json (only if the board changed)
python enclosure/build_enclosure.py   # STLs, fit report, viewer data -> enclosure/out/
python enclosure/build_viewer.py      # enclosure/out/viewer.html (interactive, needs internet for three.js)
```

Renders: [`renders/`](renders/). Fit report: [`out/checks.txt`](out/checks.txt).

## Parts

| Part | Size (mm) | Job |
|---|---|---|
| `block` | 50.5 x 143.4 x 6.0, ~10 g | Solid plinth under the PCB. 4 pilot holes (2.1 dia x 5 deep, self-tapping M2.5) match the PCB mounting holes. 48 wells (2.4 wide x 3.2 deep) take the pin tails of all 128 through-hole pads, so the board sits flat. Fills the lid footprint (0.2 mm gap each side). Front face flush with the shroud edge. 4 peg sockets for the lid. Tape it to the shroud. |
| `lid` | 52.3 x 147.0 x 26.6, ~7 g | Closed cover over the whole board and the keyfob. Air in at the south (USB-C) end, out at the north end; no vents in the roof or sides. Plain rectangular box, roof 26.6 mm over the whole length. Solid roof. Openings: USB-C, 5.8 mm round LED window in the east wall (LED bent over to face the glass, 3.6 mm above panel 2), notches for the panel headers. Keyfob is wired electrically; reach J6 by lifting the lid. West/south/north walls rest on the shroud. Tape or friction. Print roof-down. |
| `mask` | 14.0 x 153.3 x 79.6 incl. pegs, ~4 g | 1 mm plate over the three panels with three rounded windows (active area + 0.6 mm margin). Rim and ribs reach back to the panel PCBs; 7 sockets take the backplate pegs. Slides on from the glass side. |
| `backplate` | 6.0 x 153.3 x 76.6 incl. pegs, ~3 g | 2.54 mm plate between the shroud face and the panel PCBs, so the screens are clamped between it and the mask and cannot slip out. A window behind each panel (4 mm border) clears the rear components; a slot passes each header. 7 x 2.0 mm pegs into the mask. |

The keyfob is taped on the block in the notch area. It is wider than the 29 mm notch,
so its west edge overhangs the block side.

## Printing

Print-ready files are in [`out/print/`](out/print/) as STL and STEP. Each one is already oriented and sitting on z = 0, and **none need supports**.

| File | On the bed | Height | Notes |
|---|---|---|---|
| `block.stl` | bottom face | 6.0 | sockets and wells all open upward |
| `lid.stl` | roof | 26.6 | posts and pegs grow up from the roof; window tops are short bridges (1.6 mm wall) |
| `mask.stl` | front face (windows) | 14.0 | rims, tabs and pegs point up; best surface lands on the visible face |
| `backplate.stl` | back face | 6.0 | flat plate, pegs point up |

PETG, 0.2 mm layers, 3 walls, 20 % infill. All parts in black.

## Joints

Round peg into round socket, 0.15 mm radial clearance. If the fit is too tight, change `PEG_CLR` and rebuild.

- **lid to block:** 4 posts run from the roof down to the block top. Two hang off the west wall and two off the north wall. Each post ends in a 2.0 mm peg, 3 mm long, that drops into a socket in the block.
- **backplate to mask:** 7 pegs, 2.0 mm, 3.5 mm long: 4 in the gap ribs between the panels, 3 in the bottom rim.
- **mask to lid:** 2 tabs on the mask top, in the gaps between the panels, carry 3.0 mm pegs, 5 mm long. They point back into sockets through the lid's east wall and a boss behind it.

Assembly: block on the shroud, then the PCB (4 x M2.5 self-tapping), then the lid from above. Hold the backplate against the shroud face and plug each screen through its header slot. Then slide the mask on from the glass side: its pegs go into the lid and the backplate pegs go into the mask.

## Before sending to a print shop

The build stops with an error if either guard fails:

- **Handedness:** USB-C must be at the top, the screens on the right and the keyfob notch bottom-left, as on the top silkscreen. KiCad's Y axis points down, so building straight from board coordinates gives a mirror image. The model is mirrored back once, and this guard checks the result.
- **Nesting:** the block must fill the lid, with 0 to 0.5 mm gap per side.

Then print `out/print/template_1to1.pdf` at 100 % (no fit-to-page) and measure its 50 mm bar. Lay the real PCB on it, components up: the holes, pads and outline must line up. This catches a mirror or a size error before anything is printed.

## Glass clearance

Gap is 1 cm. Mask face is **9.1 mm** past the shroud edge (bare panels 7.9 mm). The
shroud edge is x = 57.5 so the panels' 2.5 mm rear parts clear it by 0.24 mm.

## Dimensions

From the GMT024-08-SPI8P drawing: PCB 46.9 x 71.0, active area 37.42 x 49.66, header
plastic 2.54, pins 5.76, overall 13.5 max (5.2 mm glass to PCB rear), rear parts 2.5 max,
pin row 3.5 mm below the panel top.

Measured: right-angle display socket body 2.0 mm high, pin centre 1.0 mm above the board; mating pins 0.64 sq x 5.76 like the panel header.

Still assumed, check before printing:

- Keyfob PCB 1.0 mm and button 3.4 mm high (30.3 x 38.5 mm from ruler photos, +-1 mm)
- Active area starts 8.9 mm below the panel top edge (read off the drawing)
- Pin tails stay under 3.2 mm below the board; shroud face flat and vertical
