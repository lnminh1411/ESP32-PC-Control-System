"""
Hand-routes the display bus on F.Cu (L1, the high-speed layer right above
the In1 GND plane) as LOCKED tracks, before Freerouting runs.

Run AFTER add_pours.py and BEFORE route_board.py:

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\route_spi.py

Why not let Freerouting do it: Freerouting builds each net as a tree and
happily T-joins a trace mid-segment (star/stub topology), and it put most
of SCK/MOSI on B.Cu. The shared lines (SCK, MOSI, DC, RST) must be a true
DAISY CHAIN -- MCU -> display1 -> display2 -> display3, every segment
ending on a pad, no T-junctions -- and stay on one layer over one plane.

Rev 3 topology (module rotated 180deg, displays plugged in directly at a
47mm pitch down the east edge), all F.Cu, no vias, planar by construction:
  * U1 (module left header, now on the right) carries the bus on adjacent
    pins 18..10: SCK, MOSI (each via its THT 33R series resistor in the
    top-right corner; the bus side drops down the outer right lanes), RST, DC,
    two strap pins skipped, CS1, CS2, CS3.
  * RST/DC step up (staircase, lower line turns further east) and
    pass OVER the top of display 1 to its right side (under the socket
    body), enter each display pad from the RIGHT, leave to the LEFT, run
    down the left lanes, cross back to the right in the gap before the next
    display. Lane order is nested so nothing crosses: right lanes DC
    innermost, left lanes SCK innermost.
  * CS1/CS2/CS3 drop down the outer left lanes and enter each display's top
    pin (CS) from the left -- above that display's exit lanes.
Everything is derived from the constants below, which must match
layout_board.py's placement (XR, XD, PITCH, C1 and the resistor X).
"""
import pcbnew

BOARD_PATH = r"E:\ESP32 PC Controller\pcb\layouts\default\default.kicad_pcb"
MM = pcbnew.FromMM
TRACK_W = 0.2

XR, XD = 46.5, 57.5          # U1 pad column, display pad column
PITCH, C1 = 47.0, 18.5       # display socket pitch / display 1 centre
R33_X = 49.4                 # r8 (SCK 33R) pad 1 X, as layout_board.py
R_PITCH = 8.128              # THT resistor pad pitch
R8 = ((R33_X, 2.0), (R33_X + R_PITCH, 2.0))              # SCK: pad 1, pad 2
R9 = ((R33_X + 1.2, 5.1), (R33_X + 1.2 + R_PITCH, 5.1))  # MOSI
U1_PIN1_Y = 56.34


def u1(n):                   # U1 pad n (module rotated: pin 1 at the bottom)
    return U1_PIN1_Y - 2.54 * (n - 1)


def dpad(d, pad):            # display d (0..2) pad 1..7 (pad 1 = GND, bottom)
    return C1 + d * PITCH + 7.62 - 2.54 * (pad - 1)


# shared nets: name, display pad, source point, over-the-top Y, staircase X,
# right-lane X, left-lane X, gap-crossing Y offset. Nesting: DC innermost on
# the right, SCK innermost on the left; staircase: lower source turns further east.
# SCK/MOSI start at their 33R's pad 2 in the top-right corner, already above
# the RST/DC top lanes, and drop straight down the outer right lanes
# (top/stair = None).
SHARED = [
    # net         pad src(x, y)       top   stair  right  left   gap
    ("disp_sck",  3, R8[1],           None, None,  60.25, 56.25, 0.0),
    ("disp_mosi", 4, R9[1],           None, None,  59.75, 55.75, 0.5),
    ("disp_rst",  5, (XR, u1(16)),    8.0,  52.95, 59.25, 55.25, 1.0),
    ("disp_dc",   6, (XR, u1(15)),    8.5,  53.45, 58.75, 54.75, 1.5),
]
GAP_Y = [C1 + PITCH - 15.0, C1 + 2 * PITCH - 15.0]  # crossing band, ~4mm above next CS pin

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

# module pin -> series resistor pad 1 (separate nets: gpio12 / gpio11):
# east, then north; SCK's riser runs 1.2mm west of MOSI's pad 1
ROUTES[("U22", "1")] = [(XR, u1(18)), (R8[0][0], u1(18)), R8[0]]
ROUTES[("U23", "1")] = [(XR, u1(17)), (R9[0][0], u1(17)), R9[0]]

# chip selects: point-to-point into each display's top pin (pad 7) from the left
CS_LANE = [XD - 3.25, XD - 3.25, XD - 3.75]
for d, (pin, lane) in enumerate(zip((12, 11, 10), CS_LANE)):
    ROUTES[(f"U{4 + d}", "7")] = [(XR, u1(pin)), (lane, u1(pin)),
                                  (lane, dpad(d, 7)), (XD, dpad(d, 7))]

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
