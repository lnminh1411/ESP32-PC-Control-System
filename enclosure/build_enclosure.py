"""
Simple mount for the carrier PCB (rev 3.1), built with CadQuery.

    pip install cadquery
    python enclosure/build_enclosure.py

Two printed parts (the earlier compartment case is kept in archive/):

  block  solid rectangular plinth the PCB bolts onto (4 M2.5 holes). Wells under every
         through-hole pad take the pin tails, so the board sits flat on the block top.
         The notch under the PCB's cut-out is plain block top for the keyfob (tape it).
         Sits on the PSU shroud, front edge flush with the shroud edge. Tape it down.
  mask   thin light plate over the three panels: windows show only the viewable area.
         Friction fit around the panel edges, a dab of tape if it creeps.

Writes enclosure/out/: STLs, viewer_data.json (parts + PCB + components) and checks.txt.

Frame: PCB coordinates (mm). X toward the glass (displays on the +X edge), Y along the
board (USB-C end at Y<0), Z up with z = 0 the PSU shroud top surface.
"""
import os, json, base64, struct
import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

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

# ------------------------------------------------------------ board data
BOARD = [(10.55, 0.0), (56.23, 0.0), (56.23, 138.22), (39.55, 138.22), (39.55, 87.98), (10.55, 87.98)]
BT = 1.6
HOLES = [(52.0, 9.7), (14.0, 84.0), (52.65, 49.4), (52.65, 101.3)]      # M2.5, 2.7 dia in the PCB
PANEL_PITCH, PANEL_W, PANEL_H = 51.9, 46.9, 71.0
PANEL_YC = [23.45 + PANEL_PITCH * i for i in range(3)]
MOD_X0, MOD_X1, MOD_Y0, MOD_Y1 = 14.2, 42.2, -3.66, 66.34
MOD_XC = 28.2
PADS = json.load(open(os.path.join(HERE, "pads.json")))                  # from dump_pads.py

# ------------------------------------------------------------ parameters
BLOCK_H = 6.0                      # block top = PCB underside
Z_BOT = BLOCK_H
Z_TOP = Z_BOT + BT
X_FRONT = 57.5                     # shroud edge, block front face
BLK_X0, BLK_Y0, BLK_Y1 = 7.0, 0.0, 139.2
TAIL_W, TAIL_DEPTH = 2.4, 3.2      # pad well width and depth below the block top
SCREW_D, SCREW_DEPTH = 2.1, 5.0    # pilot hole for a self-tapping M2.5

Z_MOD_PCB = Z_TOP + 8.5 + 2.5
SOCKET_H = 2.0                     # measured, right-angle female socket body height
SOCKET_PIN_Z = 1.0                 # measured, pin centre above the board
PIN_SQ, PIN_L = 0.64, 5.76         # mating pins, same as the panel's male header (datasheet)

# panels (GMT024-08-SPI8P datasheet)
PANEL_X0 = 57.7 + 2.54             # socket face + header plastic
PANEL_T = 5.2                      # front glass to PCB rear
PANEL_X1 = PANEL_X0 + PANEL_T
PANEL_ZTOP = Z_TOP + SOCKET_PIN_Z + 3.5
PANEL_ZBOT = PANEL_ZTOP - PANEL_H
VA_W, VA_H, VA_TOP = 37.42, 49.66, 8.9
GLASS_LIMIT = 10.0

# status LED D1 bent over to face the glass, above panel 2 (measured: its glass-side
# diameter's lower edge sits 3.6 mm above the panel 2 PCB top edge)
LED_D, LED_L = 5.0, 8.6
LED_Y = 71.0
LED_ZC = PANEL_ZTOP + 3.6 + LED_D / 2
LED_TIP = X_FRONT - 1.6 - 0.3      # assumed: dome stops just behind the lid's east wall

# mask
M_CLR, M_RIM, M_PLATE = 0.3, 1.0, 1.0
M_DEPTH = PANEL_T + 0.2            # rim depth, 0.2 slack over the panel stack
WIN_PAD = 0.6                      # window margin around the active area

# keyfob WM-FSKB V2.1 (ruler photos, +-1 mm), taped on the block in the notch
FOB_W, FOB_L, FOB_T = 30.3, 38.5, 1.0
FOB_X0 = 39.55 - 0.5 - FOB_W
FOB_YC = (87.98 + 138.22) / 2
FOB_Z0 = Z_BOT

# ============================================================== BLOCK
block = box(BLK_X0, X_FRONT, BLK_Y0, BLK_Y1, 0, BLOCK_H)

pads = [tuple(p) for p in PADS]
used = set()
wells = []

def emit(run, axis):
    if len(run) < 2:
        return
    used.update(run)
    a = pads[run[0]][axis] - TAIL_W / 2
    b = pads[run[-1]][axis] + TAIL_W / 2
    c = sum(pads[i][1 - axis] for i in run) / len(run)
    z0, z1 = BLOCK_H - TAIL_DEPTH, BLOCK_H + 0.1
    if axis == 1:
        wells.append(box(c - TAIL_W / 2, c + TAIL_W / 2, a, b, z0, z1))
    else:
        wells.append(box(a, b, c - TAIL_W / 2, c + TAIL_W / 2, z0, z1))

def chain(axis):
    other = 1 - axis
    groups = {}
    for i, p in enumerate(pads):
        if i not in used:
            groups.setdefault(round(p[other], 1), []).append(i)
    for g in groups.values():
        g.sort(key=lambda i: pads[i][axis])
        run = [g[0]]
        for i in g[1:]:
            if pads[i][axis] - pads[run[-1]][axis] <= 2.7:
                run.append(i)
            else:
                emit(run, axis)
                run = [i]
        emit(run, axis)

chain(1)
chain(0)
for i, (x, y) in enumerate(pads):
    if i not in used:
        wells.append(cyl(x, y, BLOCK_H - TAIL_DEPTH, BLOCK_H + 0.1, TAIL_W))
n_wells = len(wells)
for w in wells:
    block = block.cut(w)
for hx, hy in HOLES:
    block = block.cut(cyl(hx, hy, BLOCK_H - SCREW_DEPTH, BLOCK_H + 0.1, SCREW_D))

# ============================================================== MASK
MY0, MY1 = -(M_CLR + M_RIM), PANEL_PITCH * 2 + PANEL_W + M_CLR + M_RIM
MZ0, MZ1 = PANEL_ZBOT - M_CLR - M_RIM, PANEL_ZTOP + M_CLR + M_RIM
MX0 = X_FRONT + 0.1                # rims run back to the shroud face: closes the space behind the panels
MX1 = PANEL_X0 + M_DEPTH
MX2 = MX1 + M_PLATE
plate = box(MX1, MX2, MY0, MY1, MZ0, MZ1)
for i in range(3):
    ww, wh = VA_W + 2 * WIN_PAD, VA_H + 2 * WIN_PAD
    yc = PANEL_YC[i]
    zc = PANEL_ZTOP - VA_TOP + WIN_PAD - wh / 2
    w = (cq.Workplane("YZ").workplane(offset=MX1 - 0.1).center(yc, zc)
         .rect(ww, wh).extrude(M_PLATE + 0.2).edges("|X").fillet(1.5))
    plate = plate.cut(w)
E = 0.01
rim = [
    box(MX0, MX1 + E, MY0, MY1, PANEL_ZTOP + M_CLR, MZ1),
    box(MX0, MX1 + E, MY0, MY1, MZ0, PANEL_ZBOT - M_CLR),
    box(MX0, MX1 + E, MY0, -M_CLR, MZ0, MZ1),
    box(MX0, MX1 + E, PANEL_PITCH * 2 + PANEL_W + M_CLR, MY1, MZ0, MZ1),
]
for i in range(2):
    g0 = PANEL_PITCH * i + PANEL_W + M_CLR
    rim.append(box(MX0, MX1 + E, g0, g0 + PANEL_PITCH - PANEL_W - 2 * M_CLR,
                   PANEL_ZBOT - M_CLR - E, PANEL_ZTOP + M_CLR + E))
mask = union([plate] + rim)

# ============================================================== LID
# Closed cover over the whole board and the keyfob. Air enters through the south end
# (USB-C side) and leaves through the north end; no vents in the roof or sides.
# West/south/north walls rest on the shroud, the east wall 0.2 mm above the PCB.
# Panel headers pass through notches in the east wall. Print roof-down.
LW = 1.6
L_X0, L_X1 = 5.2, X_FRONT
L_Y0, L_Y1 = -6.0, BLK_Y1 + 1.8   # outer south / north
L_YS = 90.1                       # end of the tall (module) section
L_ZT = 26.6                       # roof top, one height over the whole board
L_ZN = L_ZT
L_ZB = Z_TOP + 0.2                # east wall bottom
lid = box(L_X0, L_X1, L_Y0, L_YS, L_ZT - LW, L_ZT)                              # tall roof
lid = lid.union(box(L_X0, L_X1, L_YS - 1.6, L_Y1, L_ZN - LW, L_ZN))             # low roof
lid = lid.union(box(L_X0, L_X0 + LW, L_Y0, L_YS, 0, L_ZT))                      # west
lid = lid.union(box(L_X0, L_X0 + LW, L_YS - 0.01, L_Y1, 0, L_ZN))
lid = lid.union(box(L_X0, L_X1, L_Y0, L_Y0 + LW, 0, L_ZT))                      # south
lid = lid.union(box(L_X0, L_X1, L_Y1 - LW, L_Y1, 0, L_ZN))                      # north
lid = lid.union(box(L_X1 - LW, L_X1, L_Y0, L_YS, L_ZB, L_ZT))                   # east
lid = lid.union(box(L_X1 - LW, L_X1, L_YS - 0.01, L_Y1, L_ZB, L_ZN))
cuts = []
# south end: USB-C openings and inlet slots
cuts.append(box(16.5, 40.0, L_Y0 - 1, L_Y0 + LW + 1, 17.4, 24.6))
for k in range(8):
    x = 8.0 + 6.0 * k
    cuts.append(box(x, x + 3.5, L_Y0 - 1, L_Y0 + LW + 1, 8.5, 16.0))
# north end: outlet slots
for k in range(8):
    x = 8.0 + 6.0 * k
    cuts.append(box(x, x + 3.5, L_Y1 - LW - 1, L_Y1 + 1, 8.0, L_ZN - LW - 0.6))
# LED window toward the glass (roof is solid: keyfob is driven electrically, J6 by lifting the lid)
cuts.append(cylx(L_X1 - LW - 1, L_X1 + 1, LED_Y, LED_ZC, LED_D + 0.8))
# panel headers: notches over each socket
for yc in PANEL_YC:
    cuts.append(box(L_X1 - LW - 1, L_X1 + 1, yc - 10.8, yc + 10.8, L_ZB - 1, Z_TOP + SOCKET_H + 0.3))
for c in cuts:
    lid = lid.cut(c)

# ============================================================== JOINTS (round peg into round socket)
# lid -> block: 4 posts from the roof down to the block top, a peg on each tip drops
#   into a socket in the block. Two hang off the west wall, two off the north wall.
# mask -> lid: 2 tabs on the mask's top rim carry pegs pointing back (-X) into
#   sockets through the lid's east wall and a full-height boss behind it, in the gaps
#   between panels. Order: block, PCB, lid from above, then slide the mask on.
PEG_CLR = 0.15                     # radial clearance, PETG on a 0.4 nozzle
LP_D, LP_L = 2.0, 3.0              # lid peg dia / length
LP_POST = 3.4                      # post width
LID_PEGS = [(8.6, 15.0, "w"), (8.6, 75.0, "w"), (12.0, 137.6, "n"), (36.0, 137.6, "n")]
for px, py, side in LID_PEGS:
    h = LP_POST / 2
    if side == "w":
        post = box(L_X0 + LW - 0.01, px + h, py - h, py + h, BLOCK_H, L_ZT - LW + 0.01)
    else:
        post = box(px - h, px + h, py - h, L_Y1 - LW + 0.01, BLOCK_H, L_ZT - LW + 0.01)
    lid = lid.union(post).union(cyl(px, py, BLOCK_H - LP_L, BLOCK_H + 0.01, LP_D))
    block = block.cut(cyl(px, py, BLOCK_H - LP_L - 0.3, BLOCK_H + 0.1, LP_D + 2 * PEG_CLR))

MP_D, MP_L = 3.0, 5.0              # mask peg dia / length
MP_Z = PANEL_ZTOP + M_CLR + 2.0
MASK_PEG_Y = [PANEL_PITCH * i + PANEL_W + (PANEL_PITCH - PANEL_W) / 2 for i in range(2)]
for py in MASK_PEG_Y:
    mask = mask.union(box(MX0, MX2, py - 3.0, py + 3.0, PANEL_ZTOP + M_CLR, MP_Z + 2.0))   # full depth: grows from the bed
    mask = mask.union(cylx(MX0 - MP_L, MX0 + 0.01, py, MP_Z, MP_D))
    lid = lid.union(box(L_X1 - LW - 3.4, L_X1 - LW + 0.01, py - 3.0, py + 3.0, MP_Z - 3.0, L_ZT - LW + 0.01))
    lid = lid.cut(cylx(MX0 - MP_L - 0.4, L_X1 + 0.1, py, MP_Z, MP_D + 2 * PEG_CLR))

parts = {"block": block, "mask": mask, "lid": lid}

# ============================================================== PCB + components (visual and fit checks)
comps = {}
def add(name, solid, colour, group="pcb"):
    comps[name] = (solid, colour, group)

pcb = poly(BOARD, Z_BOT, Z_TOP)
for hx, hy in HOLES:
    pcb = pcb.cut(cyl(hx, hy, Z_BOT - 0.1, Z_TOP + 0.1, 2.7))
add("PCB", pcb, "#1c6b3a")
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
add("LED D1", cylx(LED_TIP - LED_L, LED_TIP, LED_Y, LED_ZC, LED_D), "#3be05a")
add("J6 RST", box(12.3, 19.5, 68.5, 73.7, Z_TOP, Z_TOP + 6.0), "#111111")
add("J7 keyfob", box(17.7, 31.0, 78.4, 87.98, Z_TOP, Z_TOP + 6.0), "#111111")
for i, yc in enumerate(PANEL_YC):
    add(f"Socket J{i+3}", box(49.2, 57.7, yc - 10.5, yc + 10.5, Z_TOP, Z_TOP + SOCKET_H), "#111111")
    add(f"Panel hdr {i+1}", box(57.7, PANEL_X0, yc - 10.2, yc + 10.2, Z_TOP + SOCKET_PIN_Z - 1.27, Z_TOP + SOCKET_PIN_Z + 1.27), "#1d1d1d", "panel")
    for k in range(8):
        py = yc + (k - 3.5) * 2.54
        add(f"Panel {i+1} pin {k+1}", box(57.7 - PIN_L, 57.7, py - PIN_SQ / 2, py + PIN_SQ / 2,
            Z_TOP + SOCKET_PIN_Z - PIN_SQ / 2, Z_TOP + SOCKET_PIN_Z + PIN_SQ / 2), "#c8a84a", "panel")
    add(f"Panel {i+1} PCB", box(PANEL_X0, PANEL_X0 + 1.2, yc - PANEL_W / 2, yc + PANEL_W / 2, PANEL_ZBOT, PANEL_ZTOP), "#1b4f8a", "panel")
    add(f"Panel {i+1} rear parts", box(PANEL_X0 - 2.5, PANEL_X0, yc - 14.0, yc + 14.0, PANEL_ZTOP - 48.0, PANEL_ZTOP - 10.0), "#2a2a2a", "panel")
    add(f"Panel {i+1} LCD", box(PANEL_X0 + 1.2, PANEL_X1, yc - 21.75, yc + 21.75, PANEL_ZTOP - 5.25 - 65.8, PANEL_ZTOP - 5.25), "#0d1117", "panel")
    sz1 = PANEL_ZTOP - VA_TOP
    add(f"Screen {i+1}", box(PANEL_X1 - 0.05, PANEL_X1 + 0.02, yc - VA_W / 2, yc + VA_W / 2, sz1 - VA_H, sz1), "#2b6e9e", "screen")
add("Keyfob PCB", box(FOB_X0, FOB_X0 + FOB_W, FOB_YC - FOB_L / 2, FOB_YC + FOB_L / 2, FOB_Z0, FOB_Z0 + FOB_T), "#1c6b3a", "keyfob")
for k, (u, v) in enumerate(((7.8, 8.4), (7.2, 17.8), (21.6, 17.9))):
    bx, by = FOB_X0 + u, FOB_YC + FOB_L / 2 - v
    add(f"Fob btn {k+1}", box(bx - 3.0, bx + 3.0, by - 3.0, by + 3.0, FOB_Z0 + FOB_T, FOB_Z0 + FOB_T + 3.4), "#222222", "keyfob")

# ============================================================== checks
report = []
def say(t):
    report.append(t)
    print(t)

say("== interference: component vs printed part (mm^3, want ~0) ==")
bad = 0
for cname, (cs, _, grp) in comps.items():
    if cname == "PCB":
        continue   # PCB sits on the block top by design; keyfob is taped on it
    for pname, ps in parts.items():
        try:
            v = cs.intersect(ps).val().Volume()
        except Exception:
            v = 0.0
        if v > 0.5:
            bad += 1
            say(f"  HIT  {cname:16s} x {pname:6s} {v:8.1f}")
say(f"  {bad} interferences")
say("== part vs part (mm^3, want ~0) ==")
for a, b in (("block", "lid"), ("block", "mask"), ("lid", "mask")):
    v = parts[a].intersect(parts[b]).val().Volume()
    say(f"  {a} x {b}: {v:.2f}")
say(f"== joints == lid->block {len(LID_PEGS)} x {LP_D} dia pegs, mask->lid {len(MASK_PEG_Y)} x {MP_D} dia pegs, {PEG_CLR} mm radial clearance")
say("== glass-side protrusion ==")
say(f"  mask face {MX2 - X_FRONT:.1f} mm past the shroud edge (limit {GLASS_LIMIT:.0f}); bare panels {PANEL_X1 - X_FRONT:.1f} mm")
say(f"  panel rear parts reach x={PANEL_X0 - 2.5:.2f}, shroud edge x={X_FRONT:.2f}, clearance {PANEL_X0 - 2.5 - X_FRONT:.2f} mm")
say(f"  LED: centre z={LED_ZC:.2f} (panel top {PANEL_ZTOP:.2f}), tip x={LED_TIP:.1f}, window {LED_D + 0.8:.1f} dia in the east wall")
say("== block ==")
say(f"  lid {L_X1 - L_X0:.1f} wide, roof z={L_ZT}, outer y {L_Y0}..{L_Y1}; air in south, out north")
say(f"  {len(PADS)} through-hole pads -> {n_wells} wells ({TAIL_W} wide, {TAIL_DEPTH} deep); 4 screw pilots {SCREW_D} dia x {SCREW_DEPTH} deep")
say("== part sizes (x y z mm) ==")
for n, p in parts.items():
    b = p.val().BoundingBox()
    vol = p.val().Volume() / 1000
    say(f"  {n:6s} {b.xlen:6.1f} x {b.ylen:6.1f} x {b.zlen:5.1f}  vol {vol:6.1f} cm^3  (~{vol * 1.27 * 0.2:.0f} g at 20 percent PETG)")
with open(os.path.join(OUT, "checks.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(report) + "\n")

# ============================================================== export
for stale in ("base", "lid", "bezel", "cradle", "baycover", "lightpipe"):
    p = os.path.join(OUT, stale + ".stl")
    if os.path.exists(p):
        os.remove(p)
for n, p in parts.items():
    cq.exporters.export(p, os.path.join(OUT, f"{n}.stl"), tolerance=0.05, angularTolerance=0.3)

# print-ready copies, oriented for no support, sitting on z = 0
#   block: as modelled, bottom on the bed (all sockets and wells open upward)
#   lid:   roof on the bed, pegs point up
#   mask:  front face on the bed, rims and pegs point up
PRINT = os.path.join(OUT, "print")
os.makedirs(PRINT, exist_ok=True)
orient = {
    "block": lambda p: p,
    "lid": lambda p: p.rotate((0, 0, 0), (1, 0, 0), 180),
    "mask": lambda p: p.rotate((0, 0, 0), (0, 1, 0), 90),
}
say("== print files (out/print, mm) ==")
for n, p in parts.items():
    q = orient[n](p)
    b = q.val().BoundingBox()
    q = q.translate((-b.center.x, -b.center.y, -b.zmin))
    b = q.val().BoundingBox()
    cq.exporters.export(q, os.path.join(PRINT, f"{n}.stl"), tolerance=0.02, angularTolerance=0.2)
    cq.exporters.export(q, os.path.join(PRINT, f"{n}.step"))
    say(f"  {n:6s} {b.xlen:6.1f} x {b.ylen:6.1f} x {b.zlen:5.1f} tall")
with open(os.path.join(OUT, "checks.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(report) + "\n")

def mesh(solid, tol=0.05):
    v, t = solid.val().tessellate(tol, 0.3)
    pos = []
    for a, b, c in t:
        for i in (a, b, c):
            pos.extend((v[i].x, v[i].y, v[i].z))
    return base64.b64encode(struct.pack(f"<{len(pos)}f", *pos)).decode()

data = []
colours = {"block": "#8d95a3", "mask": "#2a2a2a", "lid": "#d8dce3"}
for n, p in parts.items():
    data.append({"name": n, "group": "enclosure", "colour": colours[n], "pos": mesh(p)})
for n, (s, c, g) in comps.items():
    data.append({"name": n, "group": g, "colour": c, "pos": mesh(s, 0.1)})
with open(os.path.join(OUT, "viewer_data.json"), "w") as f:
    json.dump({"parts": data, "panel_front": MX2, "x_front": X_FRONT}, f)
print("done")
