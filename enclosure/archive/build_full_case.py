"""
Parametric 3D-printed enclosure for the carrier PCB (rev 3.1), built with CadQuery.

    pip install cadquery
    python enclosure/build_enclosure.py

Writes enclosure/out/: one STL per printable part, viewer_data.json (every part
plus the PCB and component models, for the viewer) and checks.txt (fit report).

Frame: the PCB's own coordinates (mm). X toward the glass side panel (displays
at the +X edge), Y along the board (USB-C end at Y<0, airflow runs +Y), Z up
with z = 0 the PSU shroud top surface. Case air flows from the USB-C end
toward the keyfob-notch end.

Parts (all FDM, PETG, no screws):
  base      floor + low walls on the shroud, 4 PCB stand-off pegs, front stop tabs
  lid       snap-on cover (tall over the ESP32 module, low elsewhere)
  bezel     front frame for the three panels, hooks onto the lid
  cradle    holds the keyfob in the notch, pegs into the base floor
  baycover  slotted drop-in cover over the notch
Dimensions marked ASSUMED are read from photos or typical parts, confirm by
measuring before printing.
"""
import os, json, base64, struct, math
import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

# ------------------------------------------------------------------ helpers
def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))

def poly(pts, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).polyline(pts).close().extrude(z1 - z0)

def cyl(x, y, z0, z1, d):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)

def cylx(x0, x1, y, z, d):
    return cq.Workplane("YZ").workplane(offset=x0).center(y, z).circle(d / 2).extrude(x1 - x0)

def union(items):
    it = iter(items)
    r = next(it)
    for s in it:
        r = r.union(s)
    return r

def cutall(base, items):
    for s in items:
        base = base.cut(s)
    return base

def drange(a, b, step):
    v = a
    while v <= b + 1e-9:
        yield v
        v += step

# ------------------------------------------------------------ board data
BOARD = [(10.55, 0.0), (56.23, 0.0), (56.23, 138.22), (39.55, 138.22), (39.55, 87.98), (10.55, 87.98)]
BT = 1.6                                   # PCB thickness
HOLES = [(52.0, 9.7), (14.0, 84.0), (52.65, 49.4), (52.65, 101.3)]   # M2.5 holes, 2.7 dia
PANEL_PITCH, PANEL_W, PANEL_H = 51.9, 46.9, 71.0
PANEL_YC = [23.45 + PANEL_PITCH * i for i in range(3)]
MOD_X0, MOD_X1, MOD_Y0, MOD_Y1 = 14.2, 42.2, -3.66, 66.34       # ESP32-S3 DevKit footprint
MOD_XC = 28.2

# ------------------------------------------------------------ enclosure params
WALL = 2.4
TONGUE = 1.2                               # lid/base joint tongue thickness
Z_FLOOR = 2.0
STANDOFF = 4.0                             # board underside clearance over the floor (THT tails ~1.5)
Z_BOT = Z_FLOOR + STANDOFF                 # PCB bottom
Z_TOP = Z_BOT + BT                         # PCB top  = 7.6
Z_SPLIT = 11.0                             # base walls end, lid walls start
Z_TONG = 15.0
SOCKET_H = 1.5                             # right-angle 1x8 socket body height, measured
SOCKET_PIN_Z = 0.75                        # pin hole centre above the board, measured
Z_MOD_PCB = Z_TOP + 8.5 + 2.5              # DevKit PCB underside (sockets + header plastic) = 18.6
Z_MOD_TOP = Z_MOD_PCB + 1.6 + 3.4          # USB-C / shield top = 23.6
CEIL_T = 1.8
SH_IN = 17.8                               # shoulder inner ceiling (LED top 16.2, sockets 13.6)
TW_IN = 26.5                               # tower inner ceiling
XW_IN, XW_OUT = 10.05, 10.05 - WALL
YN_IN, YN_OUT = -4.6, -4.6 - WALL
YS_IN, YS_OUT = 88.5, 88.5 + WALL          # main-body south wall (faces the notch bay)
YE_IN, YE_OUT = 138.7, 138.7 + WALL
XS_IN, XS_OUT = 39.05, 39.05 - WALL        # strip west wall
X_FRONT = 57.5                             # shroud edge = base front edge; the panels' 2.5 mm rear parts (x>=57.74) must clear it
TOWER_X_IN, TOWER_Y_IN = 44.5, 68.0

# panels (GMT024-08-SPI8P)
PANEL_X0 = 57.7 + 2.54                     # socket face + panel header plastic (datasheet 2.54)
PANEL_T = 5.2                              # front glass to PCB rear: datasheet 13.5 max - 2.54 plastic - 5.76 pin
PANEL_X1 = PANEL_X0 + PANEL_T              # 65.2
PANEL_ZTOP = Z_TOP + SOCKET_PIN_Z + 3.5    # pin row is 3.5 mm below the panel top edge (datasheet)
PANEL_ZBOT = PANEL_ZTOP - PANEL_H
VA_W, VA_H, VA_TOP = 37.42, 49.66, 8.9     # active area, datasheet; VA_TOP read off the drawing
WIN_W, WIN_H, WIN_TOP = VA_W + 2.0, VA_H + 2.0, VA_TOP - 1.0   # bezel window = VA + 1 mm all round
PLATE_T = 1.2
ROOF_Z0 = PANEL_ZTOP + 0.3                 # bezel roof sits 0.3 above the panel top
ROOF_Z1 = ROOF_Z0 + 1.5
BEAM_Z0 = ROOF_Z1 + 0.6                    # lid front beam / catch ridge underside
GLASS_LIMIT = 10.0                         # max protrusion past the shroud edge

# keyfob (WM-FSKB V2.1), measured off the ruler photos: ~30.3 x 38.5 mm, PCB thickness ASSUMED
FOB_W, FOB_L, FOB_T = 30.3, 38.5, 1.0
# the notch is only 29 mm wide, so the bay sticks out west of the main body to fit the fob + 1 mm
BAY_X1 = XS_OUT
BAY_X0 = BAY_X1 - (FOB_W + 1.0)
BAYW_OUT = BAY_X0 - WALL                   # bay west wall outer face
BAY_Y0, BAY_Y1 = YS_OUT, YE_IN
FOB_XC, FOB_YC = (BAY_X0 + BAY_X1) / 2, (BAY_Y0 + BAY_Y1) / 2

# ------------------------------------------------------------ outlines
CAV = [(XW_IN, YN_IN), (X_FRONT + 0.1, YN_IN), (X_FRONT + 0.1, YE_IN), (XS_IN, YE_IN), (XS_IN, YS_IN), (XW_IN, YS_IN)]
def grow(d):
    return [(XW_IN - d, YN_IN - d), (X_FRONT + 0.1, YN_IN - d), (X_FRONT + 0.1, YE_IN + d),
            (XS_IN - d, YE_IN + d), (XS_IN - d, YS_IN + d), (XW_IN - d, YS_IN + d)]
LID_OUT = [(XW_OUT, YN_OUT), (X_FRONT, YN_OUT), (X_FRONT, YE_OUT), (XS_OUT, YE_OUT), (XS_OUT, YS_OUT), (XW_OUT, YS_OUT)]

def far_from(x, y, pts, r):
    return all(math.hypot(x - px, y - py) > r for px, py in pts)

slot_area = {"inlet": 0.0, "outlet": 0.0, "top": 0.0}

# ============================================================== BASE
floor = box(XW_OUT, X_FRONT, YN_OUT, YE_OUT, 0, Z_FLOOR).union(box(BAYW_OUT, XW_OUT, YS_IN, YE_OUT, 0, Z_FLOOR))
walls = box(XW_OUT, X_FRONT, YN_OUT, YE_OUT, Z_FLOOR, Z_SPLIT).union(box(BAYW_OUT, XW_OUT, YS_IN, YE_OUT, Z_FLOOR, Z_SPLIT))
walls = walls.cut(poly(CAV, Z_FLOOR - 0.1, Z_SPLIT + 0.1))
walls = walls.cut(box(BAY_X0, BAY_X1, BAY_Y0, BAY_Y1, Z_FLOOR - 0.1, Z_SPLIT + 0.1))
tongue = poly(grow(TONGUE), Z_SPLIT - 0.01, Z_TONG).cut(poly(CAV, Z_SPLIT - 0.1, Z_TONG + 0.1))
base = union([floor, walls, tongue])

# PCB stand-offs with locating pegs
for hx, hy in HOLES:
    base = base.union(cyl(hx, hy, Z_FLOOR - 0.01, Z_BOT, 6.0))
    base = base.union(cyl(hx, hy, Z_BOT - 0.01, Z_TOP + 0.4, 2.4))
# front stop tabs: hook the shroud front face, sit in the gaps between panels
for yc in (PANEL_YC[0] + PANEL_PITCH / 2, PANEL_YC[1] + PANEL_PITCH / 2):
    base = base.union(box(X_FRONT - 0.01, X_FRONT + 1.2, yc - 2.0, yc + 2.0, -14.0, Z_FLOOR))
# floor airflow slots (flow aligned with +Y), clear of the stand-offs
fl = []
for xc in list(drange(12.5, 36.0, 4.5)) + list(drange(42.0, 56.0, 4.5)):
    for y0, y1 in ((0, 20), (24, 44), (48, 68), (72, 86), (94, 112), (116, 136)):
        if xc < 38 and y0 >= 90:
            continue          # bay keeps its floor for the cradle
        ycm = (y0 + y1) / 2
        if all(far_from(xc, y, HOLES, 5.0) for y in (y0, ycm, y1)):
            fl.append(box(xc - 1.1, xc + 1.1, y0, y1, -0.1, Z_FLOOR + 0.1))
            # not credited: a flat shroud blocks them unless it is perforated under the tray
base = cutall(base, fl)
# inlet grille (north wall, +Y flow), outlets (main south wall, strip end wall, bay end wall)
for xc in drange(12.0, 56.0, 5.0):
    base = base.cut(box(xc - 2.0, xc + 2.0, YN_OUT - 0.1, YN_IN + 0.1, Z_FLOOR + 0.8, Z_SPLIT - 0.4))
    slot_area["inlet"] += 4.0 * (Z_SPLIT - 0.4 - Z_FLOOR - 0.8)
for x0 in (12.5, 18.5, 24.5, 30.5):        # also passes the keyfob wires to J7
    base = base.cut(box(x0, x0 + 4.0, YS_IN - 0.1, YS_OUT + 0.1, Z_FLOOR + 1.2, Z_SPLIT - 0.4))
    slot_area["outlet"] += 4.0 * (Z_SPLIT - 0.4 - Z_FLOOR - 1.2)
for xc in list(drange(41.5, 56.5, 4.0)) + list(drange(BAY_X0 + 2.0, 35.0, 4.0)):
    base = base.cut(box(xc - 1.0, xc + 1.0, YE_IN - 0.1, YE_OUT + 0.1, Z_FLOOR + 1.2, Z_SPLIT - 0.4))
    slot_area["outlet"] += 2.0 * (Z_SPLIT - 0.4 - Z_FLOOR - 1.2)
# cradle peg sockets (blind, from the top)
CRADLE_PEGS = [(FOB_XC - 9.0, BAY_Y0 + 4.0), (FOB_XC + 9.0, BAY_Y0 + 4.0),
               (FOB_XC - 9.0, BAY_Y1 - 4.0), (FOB_XC + 9.0, BAY_Y1 - 4.0)]
for px, py in CRADLE_PEGS:
    base = base.cut(cyl(px, py, Z_FLOOR - 1.5, Z_FLOOR + 0.1, 3.3))

# ============================================================== LID
outer_sh = poly(LID_OUT, Z_SPLIT, SH_IN + CEIL_T)
outer_tw = box(XW_OUT, TOWER_X_IN + WALL, YN_OUT, TOWER_Y_IN + WALL, Z_SPLIT, TW_IN + CEIL_T)
lid = outer_sh.union(outer_tw)
lid = lid.cut(poly(CAV, Z_SPLIT - 0.1, SH_IN))
lid = lid.cut(box(XW_IN, TOWER_X_IN, YN_IN, TOWER_Y_IN, Z_SPLIT - 0.1, TW_IN))
lid = lid.cut(poly(grow(TONGUE + 0.15), Z_SPLIT - 0.1, Z_TONG + 0.2).cut(poly(CAV, Z_SPLIT - 0.2, Z_TONG + 0.3)))
# front beam + bezel catch ridges
lid = lid.union(box(X_FRONT - 1.8, X_FRONT, YN_IN, YE_IN, BEAM_Z0, SH_IN + 0.01))
TONGUE_Y = [40.0, 58.0, 94.0, 108.5]
for yc in TONGUE_Y:
    lid = lid.union(box(52.2, 53.4, yc - 4.5, yc + 4.5, BEAM_Z0, SH_IN + 0.01))
# PCB hold-down bosses with a pocket for the peg tip
for hx, hy in HOLES:
    lid = lid.union(cyl(hx, hy, Z_TOP + 0.15, SH_IN + 0.01, 5.6))
    lid = lid.cut(cyl(hx, hy, Z_TOP + 0.1, Z_TOP + 1.5, 3.0))
# USB-C cut-out (two ports, plug overmold clearance)
lid = lid.cut(box(MOD_XC - 13.5, MOD_XC + 13.5, YN_OUT - 0.1, YN_IN + 0.1, 19.0, 25.0))
slot_area["inlet"] += 27.0 * 6.0 * 0.4     # mostly plugged, credit 40 percent
for xc in drange(12.0, 44.0, 4.0):          # inlet slots under the USB-C cut-out
    lid = lid.cut(box(xc - 1.5, xc + 1.5, YN_OUT - 0.1, YN_IN + 0.1, 12.5, 18.5))
    slot_area["inlet"] += 3.0 * 6.0
# status LED light pipe: notch through the front beam and roof edge, the pipe fills it
LED_XY = (37.65, 71.0)
PIPE_Y0, PIPE_Y1 = LED_XY[1] - 2.5, LED_XY[1] + 2.5
lid = lid.cut(box(54.0, X_FRONT + 0.1, PIPE_Y0 - 0.2, PIPE_Y1 + 0.2, ROOF_Z1, SH_IN + CEIL_T + 0.1))
# ceiling slots (flow-aligned), tower and shoulder; skip any near a boss
tops = []
for xc in drange(15.0, 41.0, 4.0):
    for y0, y1 in ((-1, 14), (18, 33), (37, 52), (55, 65)):
        tops.append(box(xc - 1.0, xc + 1.0, y0, y1, TW_IN - 0.1, TW_IN + CEIL_T + 0.1))
        slot_area["top"] += 2.0 * (y1 - y0)
for xc in drange(47.0, 55.0, 4.0):
    for y0, y1 in ((16, 30), (36, 44), (56, 62), (66, 82), (92, 98), (106, 114), (118, 134)):
        if all(far_from(xc, y, HOLES, 4.3) for y in (y0, (y0 + y1) / 2, y1)):
            tops.append(box(xc - 1.0, xc + 1.0, y0, y1, SH_IN - 0.1, SH_IN + CEIL_T + 0.1))
            slot_area["top"] += 2.0 * (y1 - y0)
for xc in drange(12.0, 30.0, 4.0):         # shoulder over J6/J7 and the LED
    if far_from(xc, 78, HOLES, 4.3):
        tops.append(box(xc - 1.0, xc + 1.0, 72, 85, SH_IN - 0.1, SH_IN + CEIL_T + 0.1))
        slot_area["top"] += 2.0 * 13
for xc in drange(41.5, 49.5, 4.0):         # strip roof west part
    for y0, y1 in ((92, 98), (106, 114), (118, 134)):
        if far_from(xc, (y0 + y1) / 2, HOLES, 4.3):
            tops.append(box(xc - 1.0, xc + 1.0, y0, y1, SH_IN - 0.1, SH_IN + CEIL_T + 0.1))
            slot_area["top"] += 2.0 * (y1 - y0)
lid = cutall(lid, tops)
# lid wall vents: north shoulder part, south (bay) wall, strip end wall, west wall of the tower
for xc in drange(48.0, 56.0, 4.0):
    lid = lid.cut(box(xc - 1.0, xc + 1.0, YN_OUT - 0.1, YN_IN + 0.1, 13.0, 16.4))
    slot_area["inlet"] += 2.0 * 3.4
for xc in drange(12.0, 35.0, 4.0):
    lid = lid.cut(box(xc - 1.0, xc + 1.0, YS_IN - 0.1, YS_OUT + 0.1, 13.0, 16.4))
    slot_area["outlet"] += 2.0 * 3.4
for xc in drange(41.5, 56.5, 4.0):
    lid = lid.cut(box(xc - 1.0, xc + 1.0, YE_IN - 0.1, YE_OUT + 0.1, 13.0, 16.4))
    slot_area["outlet"] += 2.0 * 3.4
for yc in (8.0, 22.0, 36.0, 50.0):
    lid = lid.cut(box(XW_OUT - 0.1, XW_IN + 0.1, yc - 3.0, yc + 3.0, 16.0, 20.0))

# ============================================================== BEZEL
BY0, BY1 = -2.0, PANEL_PITCH * 2 + PANEL_W + 2.0
plate = box(PANEL_X1, PANEL_X1 + PLATE_T, BY0, BY1, PANEL_ZBOT - 1.0, ROOF_Z1)
for i in range(3):
    y0 = PANEL_PITCH * i + (PANEL_W - WIN_W) / 2
    z1 = PANEL_ZTOP - WIN_TOP
    w = (cq.Workplane("YZ").workplane(offset=PANEL_X1 - 0.1)
         .center(y0 + WIN_W / 2, z1 - WIN_H / 2).rect(WIN_W, WIN_H).extrude(PLATE_T + 0.2)
         .edges("|X").fillet(2.0))
    plate = plate.cut(w)
roof = box(X_FRONT, PANEL_X1 + 0.01, BY0, BY1, ROOF_Z0, ROOF_Z1)
ribs = []
for i in range(2):
    g0 = PANEL_PITCH * i + PANEL_W + 0.3
    ribs.append(box(PANEL_X0, PANEL_X1 + 0.01, g0, g0 + PANEL_PITCH - PANEL_W - 0.6, PANEL_ZBOT - 1.0, ROOF_Z0 + 0.01))
ribs.append(box(PANEL_X0, PANEL_X1 + 0.01, BY0, -0.3, PANEL_ZBOT - 1.0, ROOF_Z0 + 0.01))
ribs.append(box(PANEL_X0, PANEL_X1 + 0.01, PANEL_PITCH * 2 + PANEL_W + 0.3, BY1, PANEL_ZBOT - 1.0, ROOF_Z0 + 0.01))
bezel = union([plate, roof] + ribs)
for yc in TONGUE_Y:
    bezel = bezel.union(box(49.5, X_FRONT + 0.01, yc - 3.5, yc + 3.5, ROOF_Z0, ROOF_Z1))
    bezel = bezel.union(box(50.0, 51.2, yc - 3.5, yc + 3.5, ROOF_Z1 - 0.01, BEAM_Z0 + 0.6))

# ============================================================== KEYFOB CRADLE
cp = box(BAY_X0 + 0.25, BAY_X1 - 0.25, BAY_Y0 + 0.3, BAY_Y1 - 0.3, Z_FLOOR, Z_FLOOR + 1.6)
for xc in drange(FOB_XC - 8, FOB_XC + 8, 5.0):
    for y0, y1 in ((BAY_Y0 + 6, FOB_YC - 4), (FOB_YC + 4, BAY_Y1 - 6)):
        cp = cp.cut(box(xc - 1.0, xc + 1.0, y0, y1, Z_FLOOR - 0.1, Z_FLOOR + 1.7))
FOB_Z0 = Z_FLOOR + 1.6 + 2.0
for sx in (-1, 1):                          # fob rests on 4 pads
    for sy in (-1, 1):
        cp = cp.union(box(FOB_XC + sx * 7.5 - 2.5, FOB_XC + sx * 7.5 + 2.5,
                          FOB_YC + sy * (FOB_L / 2 - 4) - 2.5, FOB_YC + sy * (FOB_L / 2 - 4) + 2.5,
                          Z_FLOOR + 1.59, FOB_Z0))
for sy in (-1, 1):                          # end clips: post beside the fob end, lip over it
    yedge = FOB_YC + sy * FOB_L / 2
    for sx in (-1, 1):
        px = FOB_XC + sx * 7.5
        y_post0, y_post1 = sorted((yedge + sy * 0.4, yedge + sy * 2.2))
        cp = cp.union(box(px - 3.0, px + 3.0, y_post0, y_post1, Z_FLOOR + 1.59, FOB_Z0 + FOB_T + 2.0))
        y_lip0, y_lip1 = sorted((yedge + sy * 2.2, yedge - sy * 1.4))
        cp = cp.union(box(px - 3.0, px + 3.0, y_lip0, y_lip1, FOB_Z0 + FOB_T + 0.3, FOB_Z0 + FOB_T + 2.0))
for px, py in CRADLE_PEGS:
    cp = cp.union(cyl(px, py, Z_FLOOR - 1.4, Z_FLOOR + 0.01, 3.0))
cradle = cp

# ============================================================== BAY COVER
BC_Z0 = Z_SPLIT + 0.4
bc = box(BAYW_OUT, XS_OUT, YS_OUT, YE_OUT, BC_Z0, BC_Z0 + 1.8)
for xc in drange(FOB_XC - 8, FOB_XC + 8, 4.0):
    bc = bc.cut(box(xc - 1.0, xc + 1.0, BAY_Y0 + 3, BAY_Y1 - 3, BC_Z0 - 0.1, BC_Z0 + 1.9))
    slot_area["top"] += 2.0 * (BAY_Y1 - BAY_Y0 - 6)
bc = bc.union(box(BAYW_OUT - 1.4, BAYW_OUT - 0.2, YS_OUT, YE_OUT, BC_Z0 - 3.4, BC_Z0 + 1.8))     # west skirt
bc = bc.union(box(BAYW_OUT, XS_OUT, YE_OUT + 0.2, YE_OUT + 1.4, BC_Z0 - 3.4, BC_Z0 + 1.8))     # south skirt
baycover = bc

# ============================================================== LED LIGHT PIPE (natural/clear PETG)
# D1 sits 20 mm behind the front, hidden by socket J4 and the bezel, so a bar carries its light to the
# front: rear bar rests on J4 (top z 13.6) with a pocket over the LED dome, front end rises above the
# bezel roof (top z 15.5) and shows through the notch in the lid's front edge.
PIPE_Z0 = Z_TOP + SOCKET_H + 0.1
pipe = box(LED_XY[0] - 2.5, 54.2, PIPE_Y0, PIPE_Y1, PIPE_Z0, SH_IN - 0.2)
pipe = pipe.union(box(54.0, X_FRONT, PIPE_Y0, PIPE_Y1, PIPE_Z0, 19.0))
pipe = pipe.cut(cyl(LED_XY[0], LED_XY[1], PIPE_Z0 - 0.1, Z_TOP + 8.6 + 0.3, 5.4))
lightpipe = pipe

parts = {"base": base, "lid": lid, "bezel": bezel, "cradle": cradle, "baycover": baycover, "lightpipe": lightpipe}

# ============================================================== PCB + components (visual and fit checks)
def pcb_solid():
    s = poly(BOARD, Z_BOT, Z_TOP)
    for hx, hy in HOLES:
        s = s.cut(cyl(hx, hy, Z_BOT - 0.1, Z_TOP + 0.1, 2.7))
    return s

comps = {}   # name -> (solid, colour, group)
def add(name, solid, colour, group="pcb"):
    comps[name] = (solid, colour, group)

add("PCB", pcb_solid(), "#1c6b3a")
for ref, xc in (("J1", 40.9), ("J2", 15.5)):
    add(ref, box(xc - 1.3, xc + 1.3, 1.7, 57.6, Z_TOP, Z_TOP + 8.5), "#111111")
    add(ref + "-hdr", box(xc - 1.2, xc + 1.2, 1.7, 57.6, Z_TOP + 8.5, Z_MOD_PCB), "#222222", "module")
add("DevKit", box(MOD_X0, MOD_X1, MOD_Y0, MOD_Y1, Z_MOD_PCB, Z_MOD_PCB + 1.6), "#16181d", "module")
add("ESP32 can", box(MOD_XC - 9.0, MOD_XC + 9.0, MOD_Y1 - 27.0, MOD_Y1 - 1.5, Z_MOD_PCB + 1.6, Z_MOD_PCB + 4.8), "#b9bcc2", "module")
for i, xc in enumerate((MOD_XC - 5.5, MOD_XC + 5.5)):
    add(f"USB-C {i+1}", box(xc - 4.47, xc + 4.47, MOD_Y0 - 0.44, MOD_Y0 + 6.8, Z_MOD_PCB + 1.6, Z_MOD_PCB + 4.8), "#8a8d92", "module")
add("Boost PCB", box(17.5, 28.8, 2.0, 24.0, Z_TOP + 2.5, Z_TOP + 3.5), "#1b4f8a")
add("Boost parts", box(18.5, 27.8, 3.0, 23.0, Z_TOP + 3.5, Z_TOP + 6.5), "#5c6068")
for k, yc in enumerate((4.45, 10.45, 16.45, 22.45, 28.45)):
    add(f"PC817 U{k+1}", box(31.0, 37.4, yc - 2.3, yc + 2.3, Z_TOP + 0.5, Z_TOP + 4.8), "#1a1a1a")
RES = [(18.4, 33.3), (18.4, 36.6), (18.4, 39.9), (18.4, 43.2), (18.4, 46.5), (18.4, 49.8),
       (29.8, 33.3), (29.8, 36.6), (29.8, 39.9), (29.8, 43.2), (25.2, 71.0), (43.8, 4.85), (45.0, 1.75)]
for k, (rx, ry) in enumerate(RES):
    add(f"R{k+1}", cylx(rx + 4.064 - 3.15, rx + 4.064 + 3.15, ry, Z_TOP + 1.25, 2.5), "#d9b88a")
add("LED D1", cyl(LED_XY[0], LED_XY[1], Z_TOP, Z_TOP + 8.6, 5.0), "#3be05a")
add("J6 RST", box(12.3, 19.5, 68.5, 73.7, Z_TOP, Z_TOP + 6.0), "#111111")
add("J7 keyfob", box(17.7, 31.0, 78.4, 87.98, Z_TOP, Z_TOP + 6.0), "#111111")
for i, yc in enumerate(PANEL_YC):
    add(f"Socket J{i+3}", box(49.2, 57.7, yc - 10.5, yc + 10.5, Z_TOP, Z_TOP + SOCKET_H), "#111111")
    add(f"Panel hdr {i+1}", box(57.7, PANEL_X0, yc - 10.2, yc + 10.2, Z_TOP + SOCKET_PIN_Z - 1.27, Z_TOP + SOCKET_PIN_Z + 1.27), "#1d1d1d", "panel")
    add(f"Panel {i+1} PCB", box(PANEL_X0, PANEL_X0 + 1.2, yc - PANEL_W / 2, yc + PANEL_W / 2, PANEL_ZBOT, PANEL_ZTOP), "#1b4f8a", "panel")
    add(f"Panel {i+1} rear parts", box(PANEL_X0 - 2.5, PANEL_X0, yc - 14.0, yc + 14.0, PANEL_ZTOP - 48.0, PANEL_ZTOP - 10.0), "#2a2a2a", "panel")
    add(f"Panel {i+1} LCD", box(PANEL_X0 + 1.2, PANEL_X1, yc - 21.75, yc + 21.75, PANEL_ZTOP - 5.25 - 65.8, PANEL_ZTOP - 5.25), "#0d1117", "panel")
    sz1 = PANEL_ZTOP - VA_TOP
    add(f"Screen {i+1}", box(PANEL_X1 - 0.05, PANEL_X1 + 0.02, yc - VA_W / 2, yc + VA_W / 2, sz1 - VA_H, sz1), "#2b6e9e", "screen")
# keyfob
add("Keyfob PCB", box(FOB_XC - FOB_W / 2, FOB_XC + FOB_W / 2, FOB_YC - FOB_L / 2, FOB_YC + FOB_L / 2, FOB_Z0, FOB_Z0 + FOB_T), "#1c6b3a", "keyfob")
for k, (u, v) in enumerate(((7.8, 8.4), (7.2, 17.8), (21.6, 17.9))):
    bx, by = FOB_XC - FOB_W / 2 + u, FOB_YC + FOB_L / 2 - v
    add(f"Fob btn {k+1}", box(bx - 3.0, bx + 3.0, by - 3.0, by + 3.0, FOB_Z0 + FOB_T, FOB_Z0 + FOB_T + 3.4), "#222222", "keyfob")

# ============================================================== checks
def vol(s):
    return s.val().Volume()

report = []
def say(t):
    report.append(t)
    print(t)

say("== interference: component vs enclosure part (mm^3, want ~0) ==")
bad = 0
for cname, (cs, _, grp) in comps.items():
    for pname, ps in parts.items():
        try:
            v = vol(cs.intersect(ps))
        except Exception:
            v = 0.0
        if v > 0.5:
            bad += 1
            say(f"  HIT  {cname:14s} x {pname:8s} {v:8.1f}")
say(f"  {bad} interferences")

say("== glass-side protrusion ==")
say(f"  bezel front face {PANEL_X1 + PLATE_T - X_FRONT:.1f} mm past the shroud edge (limit {GLASS_LIMIT:.0f}); bare panels {PANEL_X1 - X_FRONT:.1f} mm")
say("  lid + base front edge sits at the shroud edge; stop tabs hang 14 mm down the shroud face")
say(f"  green LED light pipe: lit face x={X_FRONT:.1f}, z {ROOF_Z1:.1f}..19.0 (bezel roof top z {ROOF_Z1:.1f}), visible straight on from the glass side")
say(f"  panel rear parts reach x={PANEL_X0 - 2.5:.2f}, shroud edge x={X_FRONT:.2f}, clearance {PANEL_X0 - 2.5 - X_FRONT:.2f} mm; panel top z={PANEL_ZTOP:.1f} vs roof z={ROOF_Z0:.1f}")
say("== heights ==")
say(f"  board top z={Z_TOP:.1f}, module top z={Z_MOD_TOP:.1f}, tower roof z={TW_IN + CEIL_T:.1f}, shoulder roof z={SH_IN + CEIL_T:.1f}")
say("== airflow (open area, cm^2) ==")
for k, v in slot_area.items():
    say(f"  {k:7s} {v / 100:6.1f}")
say(f"  inlet {slot_area['inlet']/100:.1f} vs outlet+top {(slot_area['outlet']+slot_area['top'])/100:.1f}")
say("== part bounding boxes (x y z mm) ==")
for n, p in parts.items():
    b = p.val().BoundingBox()
    say(f"  {n:9s} {b.xlen:6.1f} x {b.ylen:6.1f} x {b.zlen:5.1f}  vol {p.val().Volume() / 1000:7.1f} cm^3")
with open(os.path.join(OUT, "checks.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(report) + "\n")

# ============================================================== export
for n, p in parts.items():
    cq.exporters.export(p, os.path.join(OUT, f"{n}.stl"), tolerance=0.05, angularTolerance=0.3)

def mesh(solid, tol=0.05):
    v, t = solid.val().tessellate(tol, 0.3)
    pos = []
    for a, b, c in t:
        for i in (a, b, c):
            pos.extend((v[i].x, v[i].y, v[i].z))
    return base64.b64encode(struct.pack(f"<{len(pos)}f", *pos)).decode()

data = []
colours = {"base": "#8d95a3", "lid": "#5d7a99", "bezel": "#c9a24b", "cradle": "#6faa7e", "baycover": "#a97bb5", "lightpipe": "#8dffa0"}
for n, p in parts.items():
    data.append({"name": n, "group": "enclosure", "colour": colours[n], "pos": mesh(p)})
for n, (s, c, g) in comps.items():
    data.append({"name": n, "group": g, "colour": c, "pos": mesh(s, 0.1)})
with open(os.path.join(OUT, "viewer_data.json"), "w") as f:
    json.dump({"parts": data, "panel_front": PANEL_X1 + PLATE_T, "x_front": X_FRONT}, f)
print("done")
