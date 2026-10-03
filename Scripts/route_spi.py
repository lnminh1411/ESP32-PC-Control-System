"""
Hand-routes the display bus on F.Cu (L1, the high-speed layer right above
the In1 GND plane) as LOCKED tracks, before Freerouting runs.

Run AFTER add_pours.py and BEFORE route_board.py:

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\route_spi.py

Why not let Freerouting do it: Freerouting builds each net as a tree and
happily T-joins a trace mid-segment (star/stub topology), and it put most
of SCK/MOSI on B.Cu. The shared lines (SCK, MOSI, DC) must be a true
DAISY CHAIN -- MCU -> display1 -> display2 -> display3, every segment
ending on a pad, no T-junctions -- and stay on one layer over one plane.

Rev 2.3 topology (module rotated 180deg, displays plugged in directly at a
43mm pitch down the east edge, display column only 6.25mm east of J1), all
F.Cu, no vias, planar by construction:
  * J1 (module left header, now on the right) carries the bus on pins
    18..8: SCK, MOSI (each via a flat THT 33R in the top-right corner),
    RST1, DC, two strap pins skipped, CS1, CS2, CS3, RST2, RST3.
  * Between J1 and the display pads run 8 vertical lanes at 0.5mm pitch
    (L1..L8, west to east): SCK riser, MOSI riser, RST1 stair, DC stair,
    CS1/CS2, DC-left, MOSI-left, SCK-left. CS3 reuses L4 below the DC stair.
  * SCK/MOSI rise (jogging east at Y=6.5/7.0) to their 33R's pad 1: MOSI's
    R9 on the top row, SCK's R8 below it. Both pad 2s sit EAST of the right
    lanes, so each bus line runs back west under its own resistor body to
    its outer right lane (under the display socket bodies); MOSI's lane
    drops between R8's pads. That row order is what lets both resistors
    lie flat this close to J1 (SCK on top would cross MOSI). RST1/DC stair up
    and pass OVER display 1 at Y=8.0/8.5 to the right lanes. Shared nets
    enter each display pad from the RIGHT, leave to the LEFT, run down the
    left lanes and cross back to the right in the gap before the next
    display. Nesting: DC innermost on the right, SCK innermost on the left.
  * RST1 only goes to display 1 (rev 2.3: one RST per display). RST2/RST3
    (GPIO16/15) are slow point-to-point lines left to Freerouting (B.Cu).
  * CS1/CS2/CS3 drop down L5/L5/L4 and enter each display's top pin (CS)
    from the left -- above that display's exit lanes.
Everything is derived from the constants below, which must match
layout_board.py's placement (XL, XR, XD, PITCH, C1, R8_PAD1/R9_PAD1).
"""
import pcbnew
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)

BOARD_PATH = os.path.join(ROOT, "pcb", "layouts", "default", "default.kicad_pcb")
MM = pcbnew.FromMM
TRACK_W = 0.2

XL = 15.5                    # J2 column, as layout_board.py
XR = XL + 25.4               # J1 pad column
XD = XR + 6.25               # display pad column
PITCH, C1 = 43.0, 18.5       # display socket pitch / display 1 centre
R8_PAD1 = (XR + 2.9, 4.85)  # SCK 33R pad 1, as layout_board.py (second row)
R9_PAD1 = (XR + 4.1, 1.75)  # MOSI 33R pad 1, as layout_board.py (top row)
R_PITCH = 8.128              # THT axial resistor pad pitch (measured)
R8 = (R8_PAD1, (R8_PAD1[0] + R_PITCH, R8_PAD1[1]))  # SCK: pad 1, pad 2
R9 = (R9_PAD1, (R9_PAD1[0] + R_PITCH, R9_PAD1[1]))  # MOSI
SCK_JOG_Y, MOSI_JOG_Y = 6.5, 7.0  # riser jogs east, below R8, above RST1/DC
J1_PIN1_Y = 56.34


def lane(i):                 # vertical lane i (1..8) between J1 and the displays
    return XR + 1.0 + 0.5 * i


def u1(n):                   # J1 pad n (module rotated: pin 1 at the bottom)
    return J1_PIN1_Y - 2.54 * (n - 1)


def dpad(d, pad):            # display d (0..2) pad 1..7 (pad 1 = GND, bottom)
    return C1 + d * PITCH + 7.62 - 2.54 * (pad - 1)


# shared nets: name, display pad, source point, over-the-top Y, stair X,
# right-lane X, left-lane X, gap-crossing Y offset.
# SCK/MOSI start at their 33R's pad 2 in the top-right corner, already above
# the DC/RST1 top runs, and go straight east to the outer right lanes
# (top/stair = None).
SHARED = [
    # net         pad src(x, y)        top   stair    right      left     gap
    ("disp_sck",  3, R8[1],            None, None,    XD + 2.75, lane(8), 0.0),
    ("disp_mosi", 4, R9[1],            None, None,    XD + 2.25, lane(7), 0.5),
    ("disp_dc",   6, (XR, u1(15)),     8.5,  lane(4), XD + 1.25, lane(6), 1.5),
]
GAP_Y = [C1 + PITCH - 15.0, C1 + 2 * PITCH - 15.0]  # crossing band, ~7mm above next CS pin

ROUTES = {}
for net, pad, (sx, sy), top, stair, right, left, goff in SHARED:
    if top is None:
        pts = [(sx, sy), (right, sy)]
    else:
        pts = [(sx, sy), (stair, sy), (stair, top), (right, top)]
    for d in range(3):
        y = dpad(d, pad)
        pts += [(right, y), (XD, y)]               # enter from the right
        if d < 2:
            g = GAP_Y[d] + goff
            pts += [(left, y), (left, g), (right, g)]  # leave left, cross back
    ROUTES[net] = pts

# RST1: display 1 only -- over the top at Y=8.0, in from the right, ends there
ROUTES["disp_rst1"] = [(XR, u1(16)), (lane(3), u1(16)), (lane(3), 8.0),
                       (XD + 1.75, 8.0), (XD + 1.75, dpad(0, 5)), (XD, dpad(0, 5))]

# module pin -> series resistor pad 1 (separate nets: gpio12 / gpio11):
# east to the riser lane, north, jog east, north into pad 1. MOSI's riser
# rises past R8 pad 1 on its east side to reach the top row.
ROUTES[("R8", "1")] = [(XR, u1(18)), (lane(1), u1(18)), (lane(1), SCK_JOG_Y),
                       (R8[0][0], SCK_JOG_Y), R8[0]]
ROUTES[("R9", "1")] = [(XR, u1(17)), (lane(2), u1(17)), (lane(2), MOSI_JOG_Y),
                       (R9[0][0], MOSI_JOG_Y), R9[0]]

# chip selects: point-to-point into each display's top pin (pad 7) from the left
CS_LANE = [lane(5), lane(5), lane(4)]
for d, (pin, ln) in enumerate(zip((12, 11, 10), CS_LANE)):
    ROUTES[(f"J{3 + d}", "7")] = [(XR, u1(pin)), (ln, u1(pin)),
                                  (ln, dpad(d, 7)), (XD, dpad(d, 7))]

board = pcbnew.LoadBoard(BOARD_PATH)
fps = {fp.GetReference(): fp for fp in board.GetFootprints()}

for key, pts in ROUTES.items():
    if isinstance(key, tuple):
        net = fps[key[0]].FindPadByNumber(key[1]).GetNet()
    else:
        net = board.FindNet(key)
    n = 0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if (round(x0, 4), round(y0, 4)) == (round(x1, 4), round(y1, 4)):
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(MM(x0), MM(y0)))
        t.SetEnd(pcbnew.VECTOR2I(MM(x1), MM(y1)))
        t.SetWidth(MM(TRACK_W))
        t.SetLayer(pcbnew.F_Cu)
        t.SetNet(net)
        t.SetLocked(True)  # Freerouting treats these as fixed
        board.Add(t)
        n += 1
    print(f"{net.GetNetname():12s} {n} segments")

board.Save(BOARD_PATH)
print("saved", BOARD_PATH)
