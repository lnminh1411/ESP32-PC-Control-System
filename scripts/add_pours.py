"""
Adds the copper pours to pcb/layouts/default/default.kicad_pcb:
  - B.Cu: GND pour on net "gnd" (the return plane under the F.Cu routing).
    5V/12V are routed Power-class tracks, not planes. The net names are
    declared explicitly in main.ato (`signal gnd` / `signal v5`) -- without
    them atopile names nets after a random member pin ("cathode", "vcc").
  - Antenna keepout: a rule area on all copper layers (no tracks, vias or
    zone fill) under the plugged-in module's PCB antenna, between the two
    socket rows past pin 1. Copper under the antenna detunes WiFi/BLE. Must
    exist BEFORE routing so Freerouting (via the DSN export) routes around it.

Run AFTER layout_board.py (needs final footprint positions + board outline).
Every part is through-hole, so THT pad copper exists on both layers and the
pour connects every GND pad with no explicit traces or vias.

    <kicad-python> scripts/add_pours.py
"""
import pcbnew
from board_config import BOARD_PATH

MM = pcbnew.FromMM

board = pcbnew.LoadBoard(BOARD_PATH)

# board outline corners, read back from the Edge.Cuts rectangle
# layout_board.py drew (simpler and less error-prone than recomputing the
# placement bbox again here -- single source of truth is the saved board)
xs, ys = [], []
for item in board.GetDrawings():
    if item.GetLayer() == pcbnew.Edge_Cuts and isinstance(item, pcbnew.PCB_SHAPE):
        for pt in (item.GetStart(), item.GetEnd()):
            xs.append(pt.x)
            ys.append(pt.y)
x0, x1 = min(xs), max(xs)
y0, y1 = min(ys), max(ys)
# inset slightly from the physical board edge (standard clearance so the
# copper pour doesn't run to the literal cut line)
INSET = MM(0.5)
x0, y0, x1, y1 = x0 + INSET, y0 + INSET, x1 - INSET, y1 - INSET


def add_plane(net_name, layer, zone_name):
    net = board.FindNet(net_name)
    if net is None:
        raise SystemExit(f"net '{net_name}' not found on board")
    zone = pcbnew.ZONE(board)
    zone.SetLayer(layer)
    zone.SetNetCode(net.GetNetCode())
    zone.SetZoneName(zone_name)
    outline = zone.Outline()
    outline.NewOutline()
    for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
        outline.Append(pcbnew.VECTOR2I(int(x), int(y)))
    zone.SetIsFilled(False)
    board.Add(zone)
    return zone


# B.Cu is the GND return plane; 5V/12V are routed as Power-class tracks
gnd_zone = add_plane("gnd", pcbnew.B_Cu, "GND_pour")
# 0.3mm (vs the 0.5 default) around the ~150 THT pads: more continuous
# copper under the F.Cu routing; still > the 0.2mm netclass clearance
gnd_zone.SetLocalClearance(MM(0.3))
COPPER = (pcbnew.F_Cu, pcbnew.B_Cu)
POURS = "GND pour (B.Cu, net 'gnd')"

# ---- antenna keepout ----
# Rev 2.0: module mounted rotated (USB-C at the top edge), so the antenna end
# is at the BOTTOM: socket pin 1 at Y=56.34, PCB antenna starts ~4mm past it
# and overhangs the main body's bottom edge (~Y=58.5). Keepout spans between
# the two socket columns' courtyards from Y=54.5 down past
# the edge, so no plane copper runs up to the antenna end.
# X span follows the socket columns (J2/J1 positions), so it tracks layout changes
xl = board.FindFootprintByReference("J2").GetPosition().x / 1e6
xr = board.FindFootprintByReference("J1").GetPosition().x / 1e6
# rev 3: the board continues below the module, so the keepout runs past the
# DevKit's antenna end (10mm past pin 1 = Y 66.34) to Y=67.5
ANT_X0, ANT_X1, ANT_Y0, ANT_Y1 = xl + 1.6, xr - 1.6, 54.5, 67.5
keep = pcbnew.ZONE(board)
keep.SetIsRuleArea(True)
keep.SetZoneName("ANTENNA_KEEPOUT")
lset = pcbnew.LSET()
for layer in COPPER:
    lset.AddLayer(layer)
keep.SetLayerSet(lset)
keep.SetDoNotAllowTracks(True)
keep.SetDoNotAllowVias(True)
keep.SetDoNotAllowZoneFills(True)
keep.SetDoNotAllowPads(False)
keep.SetDoNotAllowFootprints(False)
ko = keep.Outline()
ko.NewOutline()
for x, y in [(ANT_X0, ANT_Y0), (ANT_X1, ANT_Y0), (ANT_X1, ANT_Y1), (ANT_X0, ANT_Y1)]:
    ko.Append(pcbnew.VECTOR2I(MM(x), MM(y)))
board.Add(keep)

filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())

board.Save(BOARD_PATH)
print(f"added {POURS}, antenna keepout")
print("saved", BOARD_PATH)
