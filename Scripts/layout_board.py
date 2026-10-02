"""
Real component placement for pcb/layouts/default/default.kicad_pcb.

Run AFTER sync_netlist_to_board.py (needs all 20 footprints already placed
with correct nets; this script only repositions them + draws the outline/
mounting holes/copper pours, it doesn't touch netlist/pad assignment).

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\layout_board.py

Coordinates below were derived from real measured/confirmed geometry, not
library defaults:
  - N16R8 module socket row spacing: 25.4mm (1"), measured directly off the
    real part -- NOT the 22.86mm the official Espressif reference-design
    dimension PDF shows (this is a third-party/clone board; those commonly
    drift a couple mm from Espressif's own reference, as they did here).
  - Optocoupler: 0.1" pin pitch x 0.3" row spacing, measured off the real
    part -- matches the stock DIP-4_W7.62mm footprint exactly, no fix
    needed.
  - Resistor: 0.32" (8.128mm) between solder spots, measured off the real
    part -- the stock R_Axial_DIN0207...P10.16mm footprint (0.4") does NOT
    match. Fixed below by moving pad 2 in on every resistor instance
    (same drill/pad size, corrected pitch only) rather than switching to a
    different stock footprint, since no stock DIN0207 variant matches
    8.128mm and axial leads are field-bent anyway -- what has to be exact
    is the drilled hole position, not the library part name.

All footprint sizes used for spacing come from each footprint's real
F.CrtYd/B.CrtYd bounding box (read live via pcbnew below), not assumed --
same discipline as sync_netlist_to_board.py's placement fix earlier in
this project.
"""
import pcbnew

BOARD_PATH = r"E:\ESP32 PC Controller\pcb\layouts\default\default.kicad_pcb"
RESISTOR_PITCH_MM = 8.128  # 0.32" measured, replaces stock 10.16mm (0.4")

MM = pcbnew.FromMM


def courtyard_bbox(fp):
    """Return (xmin,ymin,xmax,ymax) in mm, relative to the footprint's own
    position/orientation (i.e. as if placed at (0,0) with rotation 0)."""
    xmin = ymin = 1e9
    xmax = ymax = -1e9
    pos = fp.GetPosition()
    angle = fp.GetOrientation()
    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    fp.SetOrientation(pcbnew.EDA_ANGLE(0, pcbnew.DEGREES_T))
    for item in fp.GraphicalItems():
        if item.GetLayer() in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            b = item.GetBoundingBox()
            xmin = min(xmin, b.GetLeft() / 1e6)
            xmax = max(xmax, b.GetRight() / 1e6)
            ymin = min(ymin, b.GetTop() / 1e6)
            ymax = max(ymax, b.GetBottom() / 1e6)
    fp.SetPosition(pos)
    fp.SetOrientation(angle)
    return xmin, ymin, xmax, ymax


def place(fp, x_mm, y_mm, angle_deg=0):
    fp.SetPosition(pcbnew.VECTOR2I(MM(x_mm), MM(y_mm)))
    fp.SetOrientation(pcbnew.EDA_ANGLE(angle_deg, pcbnew.DEGREES_T))


def placed_courtyard_bbox(fp):
    """Real world-space courtyard bbox at the footprint's actual current
    position/orientation (unlike courtyard_bbox() above, which always
    evaluates at angle 0 -- needed once footprints are rotated, since a
    90-degree-rotated part's real bbox has width/height swapped)."""
    xmin = ymin = 1e9
    xmax = ymax = -1e9
    for item in fp.GraphicalItems():
        if item.GetLayer() in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            b = item.GetBoundingBox()
            xmin = min(xmin, b.GetLeft() / 1e6)
            xmax = max(xmax, b.GetRight() / 1e6)
            ymin = min(ymin, b.GetTop() / 1e6)
            ymax = max(ymax, b.GetBottom() / 1e6)
    return xmin, ymin, xmax, ymax


board = pcbnew.LoadBoard(BOARD_PATH)
fps = {fp.GetReference(): fp for fp in board.GetFootprints()}

# ---- placement (pin-1 anchor coordinates, mm, + rotation) -----------------
# U4/U5/U6 (displays), U3+U7 (boost + isolated pair) are edge-mounted and
# rotated 90 degrees per user request: pin row turned from vertical to
# horizontal and pushed out near a board edge, so a cable connects from
# outside the board footprint rather than needing clearance over the
# interior -- easier to reach once this sits in an enclosure. U7 was also
# changed from a male header to a female socket in main.ato (see there).
# Right-angle connector rotation convention (determined empirically by
# placing a PinSocket_1x08_P2.54mm_Horizontal at each angle and reading
# its real courtyard bbox -- KiCad rotates clockwise for +angle in this
# coordinate frame, Y pointing down): at angle=0 the body hangs off in
# -X from the pin line; angle=90 -> body hangs in +Y (down); angle=180
# -> body hangs in +X (right). Used below so the body actually projects
# past the board edge instead of just spinning in place -- a straight
# vertical header has no such preferred direction, which is why the
# previous in-plane-rotation attempt didn't achieve this.
# Rev 3 (2026-10-02): module ROTATED 180deg -- USB-C end at the board's TOP
# edge (the only practical plug-in side once the printed enclosure is on),
# PCB antenna overhanging the BOTTOM edge (Espressif's preferred placement).
# U1 = mcu.left (the module's left header, carrying the whole display bus)
# therefore sits on the RIGHT, facing the displays; U2 = mcu.right on the
# left. Both at 180deg: pin 1 at the bottom (antenna end), pin n at
# Y = 56.34 - 2.54*(n-1).
#
# Displays plug in DIRECTLY (no cables): 37mm-wide panels, 7 pins centred,
# 10mm between panels -> 47mm socket pitch. Panel 1's top is level with the
# board top (socket centre Y=18.5), so the board grows a narrow strip down
# the east side to carry display 2/3 (an L-shaped board, the strip extends
# downward only). Sockets overhang the east edge by DISPLAY_OVERHANG so the
# panel's own right-angle header clears the board edge.
#
# Under the module (it rides ~11mm up on its sockets): the boost (rotated,
# ~7.5mm tall), all 5 optos and the 7 axial resistors -- that's what shrinks
# the main body to ~65x60mm. The bus side (east of U1) is left to
# route_spi.py; low-speed nets reach the enclosed pads via B.Cu.
XL, XR, XD = 21.1, 46.5, 57.5     # U2 column, U1 column, display pin column
PITCH = 47.0                      # display socket pitch (37mm panel + 10mm gap)
C1 = 18.5                         # display 1 socket centre (panel top at Y=0)
R33_X = 49.4                      # r8 pad 1 X (must match route_spi.py)
layout = {
    "U2":  (XL, 56.34, 180),  # mcu.right (low-speed row: optos, LED, spares)
    "U1":  (XR, 56.34, 180),  # mcu.left  (display bus row) -- 25.4mm spacing
    # displays: angle 180 -> body hangs +X, pad 1 (GND) at the bottom; the
    # anchor is pad 1 = socket centre + 7.62
    "U4":  (XD, C1 + 7.62, 180),
    "U5":  (XD, C1 + PITCH + 7.62, 180),
    "U6":  (XD, C1 + 2 * PITCH + 7.62, 180),
    # SCK / MOSI 33R THT series resistors, top-right corner above display 1
    # (no room for an 8.1mm axial beside U1). Pad 1 columns 1.2mm apart so
    # the SCK stub passes west of MOSI's pad 1; rows 3.1mm apart (courtyards).
    "U22": (R33_X, 2.0, 0),        # r8 SCK  -- courtyard clears U1's
    "U23": (R33_X + 1.2, 5.1, 0),  # r9 MOSI
    # west edge: right-angle sockets at angle 0 (body hangs -X, off the edge)
    "U7":  (10.5, 3.2, 0),    # rst_out (motherboard RST pair)
    # keyfob (1x6): bottom edge, angle 90 -> pins run +X, body hangs +Y;
    # pin row Y puts the body face flush with the module-set bottom edge
    "U8":  (2.2, 48.0, 90),
    "U9":  (15.8, 2.8, 0),    # SpareHeader (1x16), labels on its east side
    "U28": (4.2, 12.0, 0),    # led1 -- west strip, NOT under the module
    # under the module: boost at 90deg (courtyard 11.5 x 22.5), USB-C end
    "U3":  (32.81, 21.7, 90),
    # opto column beside the boost
    "U10": (35.95, 3.2, 0),   # opto1
    "U11": (35.95, 9.2, 0),   # opto2
    "U12": (35.95, 15.2, 0),  # opto3
    "U13": (35.95, 21.2, 0),  # opto4
    "U14": (35.95, 27.2, 0),  # opto5 (motherboard RST)
    # resistors: 2 columns x 4 rows below (courtyards trimmed to fit, below)
    "U15": (24.0, 33.3, 0),   # r1
    "U16": (34.55, 33.3, 0),  # r2
    "U17": (24.0, 36.6, 0),   # r3
    "U18": (34.55, 36.6, 0),  # r4
    "U19": (24.0, 39.9, 0),   # r5
    "U20": (34.55, 39.9, 0),  # r6 (status LED)
    "U21": (24.0, 43.2, 0),   # r7 10k GPIO39 pull-down
    # rev 3.1: 10k pull-downs for the door-button optos
    "U24": (34.55, 43.2, 0),  # r10 GPIO1
    "U25": (24.0, 46.5, 0),   # r11 GPIO42
    "U26": (34.55, 46.5, 0),  # r12 GPIO41
    "U27": (24.0, 49.8, 0),   # r13 GPIO40
}
for ref, (x, y, angle) in layout.items():
    place(fps[ref], x, y, angle)

# ---- fix resistor pad pitch to the real measured 8.128mm -----------------
# Done after placement: pad 1 is the anchor (world pos == footprint pos
# since these aren't rotated), so pad 2's correct world position is just
# pad1 + pitch along X.
#
# FIRST ATTEMPT AT THIS (scaling every silkscreen/fab coordinate by
# pitch-ratio 0.8) was wrong and visibly broken -- caught from a render
# screenshot showing the resistor's real 3D body hanging well past its
# own silkscreen outline. A resistor's physical body is a FIXED length
# (6.3mm here, the "L6.3mm" in the footprint name) -- shortening the pitch
# doesn't shrink the glass/ceramic body, it only shortens the straight
# lead run between the body and the bend into the pad. So the body should
# be RECENTERED between the new pad positions, not rescaled -- a pure
# shift, same amount for every graphic, not a per-point multiply.
STOCK_RESISTOR_PITCH_MM = 10.16
# Symmetric lead bend on both sides assumed (matches how these get hand-
# formed in practice): body shifts by half the pitch change.
BODY_SHIFT_MM = (STOCK_RESISTOR_PITCH_MM - RESISTOR_PITCH_MM) / 2  # 1.016mm

for ref in ("U15", "U16", "U17", "U18", "U19", "U20", "U21", "U22", "U23", "U24", "U25", "U26", "U27"):
    fp = fps[ref]
    pad1_pos = fp.GetPosition()
    for pad in fp.Pads():
        if pad.GetNumber() == "2":
            pad.SetPosition(pcbnew.VECTOR2I(pad1_pos.x + MM(RESISTOR_PITCH_MM), pad1_pos.y))
    # shift (not scale) the stock silkscreen/fab/courtyard body+lead
    # graphics so the body re-centers between the new pad positions
    angle = fp.GetOrientation()
    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    fp.SetOrientation(pcbnew.EDA_ANGLE(0, pcbnew.DEGREES_T))
    shift = MM(BODY_SHIFT_MM)
    for item in fp.GraphicalItems():
        if item.GetLayer() in (pcbnew.F_SilkS, pcbnew.F_Fab, pcbnew.F_CrtYd) and \
                isinstance(item, pcbnew.PCB_SHAPE):
            s, e = item.GetStart(), item.GetEnd()
            item.SetStart(pcbnew.VECTOR2I(s.x - shift, s.y))
            item.SetEnd(pcbnew.VECTOR2I(e.x - shift, e.y))
    # At 8.128mm pitch the 6.3mm body leaves under 1mm per side, so the stock
    # silkscreen ends up on the pads (DRC silk_over_copper x4 per resistor):
    # the lead stubs now sit inside the pads -> move them to F.Fab (NOT
    # fp.Remove(): removing graphics corrupts the next footprint's SWIG
    # wrappers -- GraphicalItems() came back as a bare SwigPyObject); the
    # body outline touches the pad edge -> inset it to clear pad + mask
    # (fab layer keeps the true 6.3mm body).
    SILK_INSET = MM(0.3)
    for item in fp.GraphicalItems():
        if item.GetLayer() != pcbnew.F_SilkS or not isinstance(item, pcbnew.PCB_SHAPE):
            continue
        if item.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            item.SetLayer(pcbnew.F_Fab)
        elif item.GetShape() == pcbnew.SHAPE_T_RECT:
            s, e = item.GetStart(), item.GetEnd()
            lo, hi = min(s.x, e.x), max(s.x, e.x)
            item.SetStart(pcbnew.VECTOR2I(lo + SILK_INSET, s.y))
            item.SetEnd(pcbnew.VECTOR2I(hi - SILK_INSET, e.y))
    # courtyard: the stock one still spans the 10.16mm lead length (12.3mm),
    # too long for two columns under the module -> trim to pads + 0.25mm
    CRT_X0, CRT_X1 = MM(-1.05), MM(RESISTOR_PITCH_MM + 1.05)
    for item in fp.GraphicalItems():
        if item.GetLayer() != pcbnew.F_CrtYd or not isinstance(item, pcbnew.PCB_SHAPE):
            continue
        st, en = item.GetStart(), item.GetEnd()
        item.SetStart(pcbnew.VECTOR2I(max(CRT_X0, min(CRT_X1, st.x)), st.y))
        item.SetEnd(pcbnew.VECTOR2I(max(CRT_X0, min(CRT_X1, en.x)), en.y))
    # VALUE (270R / 10k) printed inside the body instead of the reference,
    # so hand-assembly reads straight off the board; the column is packed at
    # 5.5mm pitch and the stock above-body text collided with the opto above
    fp.Reference().SetVisible(False)
    val = fp.Value()
    val.SetLayer(pcbnew.F_SilkS)
    val.SetVisible(True)
    val.SetPosition(pcbnew.VECTOR2I(MM(RESISTOR_PITCH_MM / 2), 0))
    val.SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
    val.SetTextThickness(MM(0.12))
    # 3D model: the stock STEP is a rigid 10.16mm-pitch part, so shifting it
    # (old approach) centred the body but left BOTH bent leads ~1mm outside
    # their holes (seen in the 3D viewer). Swap to the stock 7.62mm-pitch
    # model and stretch it in X by 8.128/7.62 (+6.7%) about pad 1: leads land
    # exactly on both pads, body grows 6.3 -> 6.7mm (visual only).
    # fp.Models() hands back COPIES on iteration -> rebuild the vector
    # (clear()+push_back()), editing in place silently does nothing.
    models = fp.Models()
    rebuilt = []
    for model in models:
        model.m_Filename = model.m_Filename.replace("_P10.16mm_", "_P7.62mm_")
        model.m_Offset.x = 0.0
        model.m_Scale.x = RESISTOR_PITCH_MM / 7.62
        rebuilt.append(model)
    models.clear()
    for model in rebuilt:
        models.push_back(model)
    fp.SetPosition(pad1_pos)
    fp.SetOrientation(angle)

# ---- board outline: L-shape ------------------------------------------------
# Main body (module + west strip) plus a strip down the east side carrying
# display 2/3. West RA sockets sit flush (fab face + EDGE_FLUSH); display
# sockets overhang the east edge by DISPLAY_OVERHANG; other parts keep MARGIN
# from their courtyard. Top edge fixed at Y=0 = panel 1's top edge.
EDGE_FLUSH = 0.4
DISPLAY_OVERHANG = 1.0  # socket face past the east edge (panel header clearance)
MARGIN = 0.3
STRIP_WEST = XD - 4.5   # strip's west edge: clears route_spi.py's CS3 lane
STRIP_REFS = ("U5", "U6")


def placed_layer_bbox(fp, layers):
    xs, ys = [], []
    for item in fp.GraphicalItems():
        if item.GetLayer() in layers and isinstance(item, pcbnew.PCB_SHAPE):
            b = item.GetBoundingBox()
            xs += [b.GetLeft() / 1e6, b.GetRight() / 1e6]
            ys += [b.GetTop() / 1e6, b.GetBottom() / 1e6]
    return min(xs), min(ys), max(xs), max(ys)


ox0 = placed_layer_bbox(fps["U7"], (pcbnew.F_Fab,))[0] - EDGE_FLUSH
ox1 = max(placed_layer_bbox(fps[r], (pcbnew.F_Fab,))[2] for r in ("U4", "U5", "U6")) - DISPLAY_OVERHANG
oy0 = 0.0
main_bottom = max(max(placed_courtyard_bbox(fps[r])[3] for r in layout
                        if r not in STRIP_REFS + ("U8",)) + MARGIN,
                  placed_layer_bbox(fps["U8"], (pcbnew.F_Fab,))[3] + EDGE_FLUSH)
oy1 = placed_courtyard_bbox(fps["U6"])[3] + MARGIN
for r in layout:
    cy = placed_courtyard_bbox(fps[r])[1]
    assert cy >= oy0, f"{r} courtyard above the top edge ({cy:.2f})"

corners = [(ox0, oy0), (ox1, oy0), (ox1, oy1), (STRIP_WEST, oy1),
           (STRIP_WEST, main_bottom), (ox0, main_bottom), (ox0, oy0)]
for (x0, y0), (x1, y1) in zip(corners, corners[1:]):
    seg = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    seg.SetStart(pcbnew.VECTOR2I(MM(x0), MM(y0)))
    seg.SetEnd(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    seg.SetLayer(pcbnew.Edge_Cuts)
    seg.SetWidth(MM(0.15))
    board.Add(seg)

print(f"board outline: {ox1-ox0:.2f} x {oy1-oy0:.2f} mm overall "
      f"(X[{ox0:.2f},{ox1:.2f}] Y[{oy0:.2f},{oy1:.2f}]); main body to "
      f"Y={main_bottom:.2f}, strip X[{STRIP_WEST:.2f},{ox1:.2f}]")

# ---- 4 corner mounting holes (M2.5, 7mm inset from each corner) ----------
# NOTE: no removal of pre-existing Edge.Cuts/mounting-hole geometry here --
# removing-while-iterating pcbnew objects corrupts other live SWIG wrapper
# references (same class of bug as the NETINFO_ITEM issue documented in
# sync_netlist_to_board.py). Always start from a fresh blank+synced board
# (make_blank_pcb.py + sync_netlist_to_board.py) before re-running this
# script, rather than re-running it in place on its own output.

# courtyard rects of every real component, for keepout checking below
comp_rects = []
for ref in layout:
    comp_rects.append(placed_courtyard_bbox(fps[ref]))
# the plugged-in module's own body isn't a footprint (only its two sockets
# are), so keep holes out from under it explicitly -- a screw there can't be
# reached with the module fitted. Generous: rows +-1.3mm, plus antenna end
# above pin 1 and USB-C end below pin 22.
comp_rects.append((XL - 1.3, -4.0, XR + 1.3, 66.4))

HOLE_CLEARANCE = 4.5  # mm, hole CENTER to nearest courtyard edge -- the
# MountingHole_2.7mm_M2.5 footprint's own courtyard is ~5.9mm across
# (~3mm radius), so a flat 2mm clearance from the hole's center point
# wasn't enough: MH3 ended up 0.76mm into U4's courtyard before this fix
# (caught via DRC, not assumed). This needs to cover the hole's own
# radius (~3mm) plus real clearance, not just treat the hole as a point.
EDGE_CLEARANCE = 3.0  # mm, hole edge to board outline


def clear_of_components(hx, hy):
    if not (ox0 + EDGE_CLEARANCE <= hx <= ox1 - EDGE_CLEARANCE and
            oy0 + EDGE_CLEARANCE <= hy <= oy1 - EDGE_CLEARANCE):
        return False
    # L-shape: below the main body only the east strip exists
    if hy > main_bottom - EDGE_CLEARANCE and hx < STRIP_WEST + EDGE_CLEARANCE:
        return False
    for rx0, ry0, rx1, ry1 in comp_rects:
        if (rx0 - HOLE_CLEARANCE <= hx <= rx1 + HOLE_CLEARANCE and
                ry0 - HOLE_CLEARANCE <= hy <= ry1 + HOLE_CLEARANCE):
            return False
    return True


def nearest_clear_spot(tx, ty):
    """2D spiral search outward from (tx,ty) -- a pure corner-diagonal
    search gets trapped running parallel to a component that spans most
    of the board (e.g. the module/spare column), since every point along
    that one line stays colliding. Scanning a full ring of angles at each
    radius escapes that."""
    import math
    if clear_of_components(tx, ty):
        return tx, ty
    for radius in [r * 0.5 for r in range(1, 61)]:
        for a in range(0, 360, 15):
            rad = math.radians(a)
            cx, cy = tx + radius * math.cos(rad), ty + radius * math.sin(rad)
            if clear_of_components(cx, cy):
                return cx, cy
    return tx, ty  # fallback -- caller should notice if this collides


# Hand-picked target zones based on this layout's actual open space (not
# blind board corners -- the module/spare column and the opto/resistor
# block both run nearly the full board height right next to two of the
# four corners, so a corner-seeking search gets trapped). Open areas in
# this layout: the strip below the module/spare column (bottom-left) and
# the area right of the displays, above the boost/opto block (upper-right).
hole_targets = [
    (5.5, 24.5),   # west strip, between LED and keyfob
    (5.5, 37.0),
    (XD + 5.5, C1 + PITCH / 2),            # east strip, between display 1/2
    (XD + 5.5, C1 + PITCH * 1.5),          # east strip, between display 2/3
]
mount_positions = []
for tx, ty in hole_targets:
    x, y = tx, ty  # exact: a spiral search could wander into the bus lanes
    if not clear_of_components(x, y):
        print(f"  ! no clear spot for a mounting hole near ({tx},{ty}) -- skipped")
        continue
    mount_positions.append((x, y))
mh_lib = r"C:\Program Files\KiCad\10.0\share\kicad\footprints\MountingHole.pretty"
for i, (x, y) in enumerate(mount_positions, start=1):
    mh = pcbnew.FootprintLoad(mh_lib, "MountingHole_2.7mm_M2.5")
    mh.SetReference(f"MH{i}")
    mh.Reference().SetVisible(False)  # MH4's ref hung off the top edge
    mh.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    board.Add(mh)

# ---- connector pin labels (silkscreen) ------------------------------------
# Every off-board connector gets per-pin labels so cables/panels can be
# dry-fitted by matching names, not by counting pins. Display sockets: the
# GMT020-02-7P lies flat in the right-angle socket DISPLAY SIDE UP -- back
# side up reverses the whole order (panel DC would land on 5V).
def add_label(text, x, y, hjust):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
    t.SetTextThickness(MM(0.12))
    t.SetHorizJustify(hjust)
    t.SetPosition(pcbnew.VECTOR2I(x, y))
    board.Add(t)


def pad_pos(ref, num):
    for p in fps[ref].Pads():
        if p.GetNumber() == str(num):
            return p.GetPosition()
    raise KeyError(f"{ref}.{num}")


LEFT, RIGHT, CENTER = (pcbnew.GR_TEXT_H_ALIGN_LEFT, pcbnew.GR_TEXT_H_ALIGN_RIGHT,
                       pcbnew.GR_TEXT_H_ALIGN_CENTER)
for ref, cs in (("U4", "CS1"), ("U5", "CS2"), ("U6", "CS3")):
    for num, name in enumerate(["GND", "5V", "SCL", "SDA", "RST", "DC", cs], start=1):
        p = pad_pos(ref, num)
        add_label(name, p.x - MM(1.6), p.y, RIGHT)
for num, name in enumerate(["12V", "B1", "B2", "B3", "B4", "GND"], start=1):
    p = pad_pos("U8", num)  # bottom-edge keyfob: labels above each pad
    add_label(name, p.x, p.y - MM(1.7), CENTER)
for num, name in enumerate(["RST", "GND"], start=1):
    p = pad_pos("U7", num)  # west-edge RST pair: labels just east of each pad
    add_label(name, p.x + MM(1.3), p.y, LEFT)  # clears U9's silk
spare_gpios = ["4", "5", "6", "7", "15", "16", "3", "46",
               "13", "14", "2", "38", "0", "45", "48", "47"]
for num, name in enumerate(spare_gpios, start=1):
    p = pad_pos("U9", num)
    add_label(name, p.x + MM(2.1), p.y, LEFT)

# top-edge parts whose stock ref would sit off the board / on a neighbour
# (pin labels already identify them)
for ref in ("U7", "U9"):
    fps[ref].Reference().SetVisible(False)

# ---- clip connector silk to the board edge ---------------------------------
# Display sockets deliberately overhang the east edge (and the west sockets
# sit flush), so their body outline would print past Edge.Cuts: clip each
# silk segment to the board, move ones entirely outside to F.Fab.
SILK_EDGE = MM(0.2)
for ref in ("U4", "U5", "U6", "U7"):
    lo, hi = MM(ox0) + SILK_EDGE, MM(ox1) - SILK_EDGE
    for item in fps[ref].GraphicalItems():
        if item.GetLayer() != pcbnew.F_SilkS or not isinstance(item, pcbnew.PCB_SHAPE):
            continue
        st, en = item.GetStart(), item.GetEnd()
        if (st.x > hi and en.x > hi) or (st.x < lo and en.x < lo):
            item.SetLayer(pcbnew.F_Fab)
            continue
        if item.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            item.SetStart(pcbnew.VECTOR2I(max(lo, min(hi, st.x)), st.y))
            item.SetEnd(pcbnew.VECTOR2I(max(lo, min(hi, en.x)), en.y))

# ---- module orientation ----------------------------------------------------
# The N16R8 plugs in ROTATED: USB-C end UP (top edge), antenna end DOWN
# (overhanging the bottom edge). Its header pin 1s sit at the BOTTOM: GND on
# the left row (module's right header), 3V3 on the right row (left header).
MID = (XL + XR) / 2
add_label("USB-C END (TOP EDGE)", MM(MID), MM(0.9), CENTER)
add_label("ESP32-S3 N16R8", MM(MID), MM(52.6), CENTER)
add_label("ANTENNA END", MM(MID), MM(53.8), CENTER)

# fit check: DevKit envelope (slab at its real height on the sockets) on U2,
# see make_fit_models.py -- visual only, shows what has to fit under it
dk = pcbnew.FP_3DMODEL()
dk.m_Filename = "${KIPRJMOD}/../../footprints/ESP32PCC.3dshapes/DevKit_N16R8_envelope.wrl"
fps["U2"].Models().push_back(dk)
p = pad_pos("U2", 1)
add_label("GND", p.x + MM(1.7), p.y, LEFT)
p = pad_pos("U1", 1)
add_label("3V3", p.x - MM(1.7), p.y, RIGHT)

# ---- optocoupler pin-1 marking ----------------------------------------------
# The fitted PC817C marks pin 1 (LED anode) with a moulded DOT in the pin-1
# corner, not a top-middle notch -> replace the footprint's notch arc with a
# straight edge (arc moved to F.Fab, never fp.Remove()) and print a dot in the
# same corner, so the part is placed dot-to-dot.
for ref in ("U10", "U11", "U12", "U13", "U14"):
    fp = fps[ref]
    pos, angle = fp.GetPosition(), fp.GetOrientation()
    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    fp.SetOrientation(pcbnew.EDA_ANGLE(0, pcbnew.DEGREES_T))
    for item in fp.GraphicalItems():
        if item.GetLayer() != pcbnew.F_SilkS or not isinstance(item, pcbnew.PCB_SHAPE):
            continue
        if item.GetShape() == pcbnew.SHAPE_T_ARC:
            item.SetLayer(pcbnew.F_Fab)
        elif item.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            st, en = item.GetStart(), item.GetEnd()
            # top-left stub of the notched top edge -> run it across the gap
            if st.y == en.y == MM(-1.33) and max(st.x, en.x) < MM(3.81):
                item.SetStart(pcbnew.VECTOR2I(MM(4.81), st.y))
    # ref inside the body: the stock above-body ref collided with the opto above
    fp.Reference().SetPosition(pcbnew.VECTOR2I(MM(3.81), MM(1.6)))
    fp.Reference().SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
    fp.Reference().SetTextThickness(MM(0.12))
    fp.SetPosition(pos)
    fp.SetOrientation(angle)
    dot = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
    dot.SetLayer(pcbnew.F_SilkS)
    dot.SetCenter(pcbnew.VECTOR2I(pos.x + MM(2.0), pos.y - MM(0.45)))
    dot.SetEnd(pcbnew.VECTOR2I(pos.x + MM(2.0) + MM(0.35), pos.y - MM(0.45)))
    dot.SetFilled(True)
    dot.SetWidth(MM(0.1))
    board.Add(dot)

board.Save(BOARD_PATH)
print("saved", BOARD_PATH)
