"""
Hand-routes the board on F.Cu (over the B.Cu GND pour) as LOCKED tracks,
before Freerouting runs.

Run AFTER add_pours.py and BEFORE route_board.py:

    <kicad-python> scripts/route_spi.py

Why not let Freerouting do it: Freerouting builds each net as a tree and
happily T-joins a trace mid-segment (star/stub topology), and it put most
of SCK/MOSI on B.Cu. The shared lines (SCK, MOSI, DC) must be a true
DAISY CHAIN -- MCU -> display1 -> display2 -> display3, every segment
ending on a pad, no T-junctions -- and stay on one layer over one plane.

Display bus topology (module rotated 180deg, displays plugged in directly at a
51.9mm pitch down the east edge, display column only 6.25mm east of J1), all
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
  * RST1 only goes to display 1 (one RST per display). RST2/RST3, BL1-3,
    the display 5V and the rest of the board are routed further down.
  * CS1/CS2/CS3 drop down L5/L5/L4 and enter each display's CS pin (pad 7)
    from the left -- above that display's exit lanes.
Everything is derived from the constants below, which must match
layout_board.py's placement (XL, XR, XD, PITCH, C1, R8_PAD1/R9_PAD1).
"""
import pcbnew
from board_config import BOARD_PATH

MM = pcbnew.FromMM
TRACK_W = 0.2

XL = 15.5                    # J2 column, as layout_board.py
XR = XL + 25.4               # J1 pad column
XD = XR + 6.25               # display pad column
PITCH, C1 = 51.9, 46.9 / 2   # display socket pitch / display 1 centre
D_PAD1 = 3.5 * 2.54          # display socket pad 1 below its centre (1x8)
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


def dpad(d, pad):            # display d (0..2) pad 1..8 (pad 1 = GND, bottom; 8 = BL)
    return C1 + d * PITCH + D_PAD1 - 2.54 * (pad - 1)


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

# ---- display-side power + per-display RST/BL, and the rest of the board ----
# B.Cu is the GND pour, so everything stays on F.Cu except one short B.Cu
# jumper per bus crossing (via -> B.Cu straight into the THT display pad):
#   * 5V: J1 pin 21 -> west of J1 (outside the antenna keepout) -> round J1's
#     bottom end -> westmost strip lane -> display 3 pad 2 (no crossings) ->
#     F.Cu trunk under the socket bodies (east of the right lanes) north to
#     display 2 / display 1 pad 2. The THT pad of display 3 is the only "hop".
#   * west lanes (pitch 0.43 beside J1, fanned out to 0.7 in the strip so the
#     jumper vias fit): RST2 (J1.9), RST3 (J1.8), BL2 (J1.6), BL3 (J1.5);
#     lower pin = western lane, so the fan-out is planar.
#   * BL1 = J1 pin 13 (GPIO3, rev 3.1): F.Cu east into the empty pocket
#     between DC's and CS1's stubs, one B.Cu jumper under DC + CS1 only (slow
#     lines), via, F.Cu into display 1 pad 8. Pin 19 (rev 2.4b-3.0) sat
#     inside the SCK/MOSI riser loop, so its jumper cut the GND pour right
#     under the two fast lines.
EXTRA = []   # (net, layer, width, pts)
VIAS = []    # (net, x, y)
PW = 0.4                             # 5V track (0.4 to squeeze past J1)
W5V_X, W5V_TURN_Y = XR - 1.3, 57.65  # west of J1 / round J1's bottom end
S5V = XR - 0.6                       # 5V strip lane
TRUNK = XD + 3.5                     # 5V trunk under the socket bodies
GAP_Y = 19.47                        # between opto pads 17.74 / 21.2 (boost tee)
# tee points get their own vertex (Freerouting doesn't see a track end
# landing mid-segment as connected)
EXTRA.append(("v5", pcbnew.F_Cu, PW, [(XR, u1(21)), (W5V_X, u1(21)), (W5V_X, GAP_Y), (W5V_X, W5V_TURN_Y),
                                      (S5V, W5V_TURN_Y), (S5V, dpad(2, 2)), (XD, dpad(2, 2))]))
EXTRA.append(("v5", pcbnew.F_Cu, PW, [(XD, dpad(2, 2)), (TRUNK, dpad(2, 2)),
                                      (TRUNK, dpad(1, 2)), (TRUNK, dpad(0, 2)), (XD, dpad(0, 2))]))
EXTRA.append(("v5", pcbnew.F_Cu, PW, [(TRUNK, dpad(1, 2)), (XD, dpad(1, 2))]))
# boost IN+ (PS1.1) sits west of the opto column, whose LED anodes rise
# from the resistors on the east side: tee off the lane between opto
# slots 2 and 3, jumper under the anode bus + output pins on B.Cu, then
# F.Cu into the pad.
PS1_IN = (XL + 11.71, 21.7)          # PS1 pad 1 (boost at 90deg)
J5V_E, J5V_W = XR - 3.9, XR - 11.9   # jumper vias east/west of the column
EXTRA.append(("v5", pcbnew.F_Cu, PW, [(W5V_X, GAP_Y), (J5V_E, GAP_Y)]))
EXTRA.append(("v5", pcbnew.B_Cu, PW, [(J5V_E, GAP_Y), (J5V_W, GAP_Y)]))
EXTRA.append(("v5", pcbnew.F_Cu, PW, [(J5V_W, GAP_Y),
                                      (PS1_IN[0], GAP_Y + J5V_W - PS1_IN[0]), PS1_IN]))
VIAS.append(("v5", J5V_E, GAP_Y))
VIAS.append(("v5", J5V_W, GAP_Y))
# rev 3: 12V collector bus (opto pins 4, U1-U4) inside the DIP corridor,
# east of the output pins -- left to Freerouting it ran down their WEST
# side and walled in the btn lines (emitters, pins 3) that must exit west.
# Boost OUT+ -> over the top of U1 -> down the corridor, a stub per pin.
OUT_POS = (XL + 3.81, 3.95)          # PS1 pad 3 (boost at 90deg)
OPTO_W = XL + 14.85                  # opto output pin column
V12_X, V12_TOP = OPTO_W + 1.4, 1.2
pin4 = [3.2 + 6 * k + 2.54 for k in range(4)]  # U1-U4 collectors
EXTRA.append(("v12", pcbnew.F_Cu, PW + 0.1, [OUT_POS, (OUT_POS[0], V12_TOP), (V12_X, V12_TOP)]
              + [(V12_X, y) for y in pin4]))
for y in pin4:
    EXTRA.append(("v12", pcbnew.F_Cu, PW + 0.1, [(V12_X, y), (OPTO_W, y)]))

# rev 3 west channel: opto outputs + 12V + the LED drive run from the
# optos west, cross J2 between its unused upper pins (10-20, one track per
# gap), drop down the channel between J2 and the west edge and fan into
# J6/J7/R6 below the antenna keepout. Entry order top-down = lane order
# west->east = the order the sockets want, so nothing crosses. Positions
# must match layout_board.py (J6_PIN1, J7_PIN1, R6, D1).
def j2(n):                           # J2 pad n (rotated like J1)
    return 56.34 - 2.54 * (n - 1)
J6P = (XL + 5.53, 69.8)              # J6 pin 1 (pin 2 at +2.54 Y)
J7P = (XL + 3.5, 77.5)               # J7 pin 1 (pins run +X)
R6P = (XL + 9.7, 71.0)               # R6 pad 1 (pad 2 at +8.128 X)
D1A = (XL + 23.4 - 2.54, 71.0)       # D1 pad 2 (anode; D1 at 180deg)
# 0.42 pitch (0.22 gaps: chamfer_corners needs > 0.21 to cut the bends);
# 12V narrows to 0.4 here (keyfob supply only), 0.55 from the board edge
LANE = {"v12": XL - 4.2, "btn1": XL - 3.68, "btn2": XL - 3.26, "btn3": XL - 2.84,
        "btn4": XL - 2.42, "mb_rst_gnd": XL - 2.0, "mb_rst": XL - 1.58, "gpio2": XL - 1.16}
# 12V: the bus's top run continues west over J2's top end (a 0.5mm track
# doesn't fit between J2 pads) into the westmost lane
EXTRA.append(("v12", pcbnew.F_Cu, PW, [(OUT_POS[0], V12_TOP), (LANE["v12"], V12_TOP)]))
# (net, opto output pad, J2 gap centre Y) -- btn4 dips round boost IN+
GAP = lambda n: j2(n) - 1.27         # gap between J2 pads n and n+1
OUTS = [
    ("btn1", (OPTO_W, 3.2), GAP(20)),        # under boost OUT-/OUT+
    ("btn2", (OPTO_W, 9.2), GAP(19)),
    ("btn3", (OPTO_W, 15.2), GAP(17)),
    ("btn4", (OPTO_W, 21.2), GAP(13)),       # south of boost IN+
    ("mb_rst_gnd", (OPTO_W, 27.2), GAP(12)),  # U5 emitter
    ("mb_rst", (OPTO_W, 29.74), GAP(11)),     # U5 collector
]
# down the channel to below the keepout, then east into each pad: J6 pads
# straight across (under J6's body), J7 lines in stacked rows between J6
# pin 2 and J7's pads, each dropping into its pin
J7_ROW = {"btn4": 73.6, "btn3": 74.1, "btn2": 74.6, "btn1": 75.1, "v12": 75.75}
END = {"mb_rst": [(LANE["mb_rst"], J6P[1]), J6P],
       "mb_rst_gnd": [(LANE["mb_rst_gnd"], J6P[1] + 2.54), (J6P[0], J6P[1] + 2.54)]}
for i, net in enumerate(("v12", "btn1", "btn2", "btn3", "btn4")):
    px = J7P[0] + 2.54 * i
    END[net] = [(LANE[net], J7_ROW[net]), (px, J7_ROW[net]), (px, J7P[1])]
for net, (ox, oy), gy in OUTS:
    if net == "btn4":   # SW past U4's pin 4 + boost IN+, then west at the gap
        head = [(ox, oy), (ox - 1.45, oy + 1.45), (ox - 1.45 - (gy - oy - 1.45), gy)]
    else:
        head = [(ox, oy), (ox - abs(gy - oy), gy)]
    EXTRA.append((net, pcbnew.F_Cu, TRACK_W, head + [(LANE[net], gy)] + END[net]))
EXTRA.append(("v12", pcbnew.F_Cu, PW, [(LANE["v12"], V12_TOP)] + END["v12"]))
# LED: J2 pin 5 -> east-most lane -> above J6 pin 1 -> R6 -> D1 anode
LED_Y = 68.2                         # between the keepout (67.5) and J6 pin 1
EXTRA.append(("gpio2", pcbnew.F_Cu, TRACK_W, [(XL, j2(5)), (LANE["gpio2"], j2(5)),
                                              (LANE["gpio2"], LED_Y), (R6P[0], LED_Y), R6P]))
EXTRA.append(("led_a", pcbnew.F_Cu, TRACK_W, [(R6P[0] + 8.128, R6P[1]), D1A]))

# rev 3 opto drive: J2 -> 270R (+ 10k pull-down) -> opto LED anode, all
# F.Cu (Freerouting kept leaving one of these on a long B.Cu run). Resistor
# rows as layout_board.py: west column pad 1 at W1, east at E1, pitch 8.128.
RP = 8.128
W1, E1 = XL + 2.9, XL + 14.3
W2, E2 = W1 + RP, E1 + RP
ROW = dict(a=33.3, b=36.6, c=39.9, d=43.2, e=46.5, f=49.8)
OPTO_E = OPTO_W + 7.62               # opto LED pins (pin 1 anode)
an = {k: 5.74 + 6 * (k - 1) for k in range(1, 6)}  # U1-U5 anode Y
# GPIO39/40/41 -> R1/R2/R3 (west a,b,c) straight from J2 pins 9/8/7; the
# pull-downs hang below: GPIO41 -> R12 (west d) straight down the pad
# column, GPIO40/39 -> R13/R7 (west e,f) down under the west bodies
EXTRA.append(("gpio39", pcbnew.F_Cu, TRACK_W, [(XL, j2(9)), (W1, ROW["a"])]))
EXTRA.append(("gpio40", pcbnew.F_Cu, TRACK_W, [(XL, j2(8)), (W1, ROW["b"])]))
EXTRA.append(("gpio41", pcbnew.F_Cu, TRACK_W, [(XL, j2(7)), (W1, ROW["c"])]))
EXTRA.append(("gpio41", pcbnew.F_Cu, TRACK_W, [(W1, ROW["c"]), (W1, ROW["d"])]))
for net, r, gap, cx, pd in (("gpio40", "b", 38.25, W1 + 2.6, "e"),
                            ("gpio39", "a", 34.95, W1 + 3.1, "f")):
    EXTRA.append((net, pcbnew.F_Cu, TRACK_W, [(W1, ROW[r]), (W1 + gap - ROW[r], gap), (cx, gap),
                                              (cx, ROW[pd]), (W1, ROW[pd])]))
# GPIO42 / GPIO1 (J2 pins 6/4): down beside J2, east under the bank, up
# either side of the east pad-1 column -> R4 + pull-down R11 (west side),
# R5 + pull-down R10 (east side)
G42_X, G1_X = E1 - 1.25, E1 + 1.2
EXTRA.append(("gpio42", pcbnew.F_Cu, TRACK_W, [(XL, j2(6)), (XL + 1.6, j2(6)), (XL + 1.6, 51.0),
                                               (G42_X, 51.0), (G42_X, ROW["b"]), (E1, ROW["b"])]))
EXTRA.append(("gpio42", pcbnew.F_Cu, TRACK_W, [(G42_X, ROW["c"]), (E1, ROW["c"])]))
EXTRA.append(("gpio1", pcbnew.F_Cu, TRACK_W, [(XL, j2(4)), (XL + 1.17, j2(4)), (XL + 1.17, 51.45),
                                              (G1_X, 51.45), (G1_X, ROW["a"]), (E1, ROW["a"])]))
EXTRA.append(("gpio1", pcbnew.F_Cu, TRACK_W, [(G1_X, ROW["d"]), (E1, ROW["d"])]))
# anodes: west column (R1-R3 -> U1-U3) north between the columns, east
# along the gap under U5, up the DIP corridor (lanes west->east U1-U3) and
# peel east onto each anode pin; R4 -> U4 up a lane east of the corridor,
# R5 -> U5 straight up
for k, r, x_up, y_turn, lane in ((1, "a", W2, 30.95, OPTO_W + 2.15),
                                 (2, "b", W2 + 1.12, 31.4, OPTO_W + 2.6),
                                 (3, "c", W2 + 1.57, 31.85, OPTO_W + 3.05)):
    pts = [(W2, ROW[r])]
    if x_up != W2:
        pts.append((x_up, ROW[r] - (x_up - W2)))
    pts += [(x_up, y_turn), (lane, y_turn), (lane, an[k]), (OPTO_E, an[k])]
    EXTRA.append((f"opto{k}-anode", pcbnew.F_Cu, TRACK_W, pts))
U4_X = OPTO_E - 1.57
EXTRA.append(("opto4-anode", pcbnew.F_Cu, TRACK_W, [(E2, ROW["b"]), (U4_X, ROW["b"] - (E2 - U4_X)),
                                                    (U4_X, an[4]), (OPTO_E, an[4])]))
EXTRA.append(("opto5-anode", pcbnew.F_Cu, TRACK_W, [(E2, ROW["a"]), (OPTO_E, an[5])]))
# west lanes: net, J1 pin, lane beside J1, fan-out Y, strip lane, display, pad, jumper?
WEST = [
    ("disp_rst2", 9, XR + 2.50, 60.5, XR + 2.3, 1, 5, True),
    ("disp_rst3", 8, XR + 2.06, 60.0, XR + 1.6, 2, 5, False),  # nothing left to cross
    ("disp_bl2",  6, XR + 1.63, 59.5, XR + 0.9, 1, 8, True),
    ("disp_bl3",  5, XR + 1.20, 59.0, XR + 0.2, 2, 8, True),
]
for net, pin, lx, fy, sx, d, pad, jump in WEST:
    y = dpad(d, pad)
    pts = [(XR, u1(pin)), (lx, u1(pin)), (lx, fy), (sx, fy + lx - sx), (sx, y)]  # 45deg fan-out
    if jump:
        EXTRA.append((net, pcbnew.F_Cu, TRACK_W, pts))
        VIAS.append((net, sx, y))
        EXTRA.append((net, pcbnew.B_Cu, TRACK_W, [(sx, y), (XD, y)]))
    else:
        EXTRA.append((net, pcbnew.F_Cu, TRACK_W, pts + [(XD, y)]))
BL1_V1 = (XR + 2.1, u1(15) + 0.72)        # pocket below DC's stub, west of CS1
BL1_V2 = (XD - 1.85, dpad(0, 8) + 1.44)   # pocket west of display 1 pad 8
EXTRA.append(("disp_bl1", pcbnew.F_Cu, TRACK_W, [(XR, u1(13)), (BL1_V1[0], u1(13)), BL1_V1]))
EXTRA.append(("disp_bl1", pcbnew.B_Cu, TRACK_W, [BL1_V1, BL1_V2]))
VIAS.append(("disp_bl1",) + BL1_V1)
VIAS.append(("disp_bl1",) + BL1_V2)
EXTRA.append(("disp_bl1", pcbnew.F_Cu, TRACK_W, [BL1_V2, (BL1_V2[0] + 1.44, dpad(0, 8)),
                                                 (XD, dpad(0, 8))]))

board = pcbnew.LoadBoard(BOARD_PATH)
fps = {fp.GetReference(): fp for fp in board.GetFootprints()}

for net_name, layer, width, pts in EXTRA:
    net = board.FindNet(net_name)
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(MM(x0), MM(y0)))
        t.SetEnd(pcbnew.VECTOR2I(MM(x1), MM(y1)))
        t.SetWidth(MM(width))
        t.SetLayer(layer)
        t.SetNet(net)
        t.SetLocked(True)
        board.Add(t)
for net_name, x, y in VIAS:
    v = pcbnew.PCB_VIA(board)
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    power = net_name == "v5"             # Power-class via (0.8/0.4)
    v.SetWidth(MM(0.8 if power else 0.6))
    v.SetDrill(MM(0.4 if power else 0.3))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(board.FindNet(net_name))
    v.SetLocked(True)
    board.Add(v)
if EXTRA:
    print(f"display side: {len(EXTRA)} runs, {len(VIAS)} jumper vias")

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
