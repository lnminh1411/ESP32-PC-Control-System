"""
Adds the two plane-layer copper pours to pcb/layouts/default/default.kicad_pcb:
  - In1 (GND_Plane): solid pour on net "gnd".
  - In2 (Power_Planes): solid pour on net "v5" (the module's 5Vin rail).
  Both names are declared explicitly in main.ato (`signal gnd` / `signal v5`)
  -- before that, atopile named them after a random member pin ("cathode",
  "vcc").
  - Antenna keepout: a rule area on ALL copper layers (no tracks, vias or
    zone fill) under the plugged-in module's PCB antenna, which sits above
    socket pin 1 between the two rows. Copper (especially the GND plane)
    directly under the antenna detunes WiFi/BLE. Must exist BEFORE routing so
    Freerouting (via the DSN export) routes around it.

Run AFTER layout_board.py (needs final footprint positions + board outline
to exist) and BEFORE exporting to Freerouting -- every part on this board
is through-hole, so THT pad copper already exists on every layer by
default; a filled zone on these two inner layers connects to same-net
pads automatically, with no explicit traces or vias required for GND/5V.

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\add_pours.py
"""
import pcbnew

BOARD_PATH = r"E:\ESP32 PC Controller\pcb\layouts\default\default.kicad_pcb"
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


gnd_zone = add_plane("gnd", pcbnew.In1_Cu, "GND_pour")
pwr_zone = add_plane("v5", pcbnew.In2_Cu, "5V_pour")

# ---- antenna keepout ----
# Rev 3: module mounted rotated (USB-C at the top edge), so the antenna end
# is at the BOTTOM: socket pin 1 at Y=56.34, PCB antenna starts ~4mm past it
# and overhangs the main body's bottom edge (~Y=59.8). Keepout spans between
# the two socket columns' courtyards (X=21.1/46.5 rows) from Y=54.5 down past
# the edge, so no plane copper runs up to the antenna end.
ANT_X0, ANT_X1, ANT_Y0, ANT_Y1 = 22.7, 44.9, 54.5, 62.0
keep = pcbnew.ZONE(board)
keep.SetIsRuleArea(True)
keep.SetZoneName("ANTENNA_KEEPOUT")
lset = pcbnew.LSET()
for layer in (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
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
print("added GND pour (In1, net 'gnd'), 5V pour (In2, net 'v5'), antenna keepout")
print("saved", BOARD_PATH)
