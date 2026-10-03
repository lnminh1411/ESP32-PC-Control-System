"""
Create a blank 4-layer KiCad board file at pcb/layouts/default/default.kicad_pcb.

WARNING: overwrites that file unconditionally, discarding any placement/
routing work already done. Only run this once, before the board exists,
or deliberately to start the layout over. After real placement/routing
has begun, do NOT re-run this -- use sync_netlist_to_board.py instead,
which only re-syncs nets onto an EXISTING board's already-placed
footprints (see its own docstring for how it handles added/removed
parts).

Usage:
    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" make_blank_pcb.py
(must run under KiCad's own bundled Python -- that's the only one with
the pcbnew module installed.)
"""
import pcbnew
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)

OUT = os.path.join(ROOT, "pcb", "layouts", "default", "default.kicad_pcb")
os.makedirs(os.path.dirname(OUT), exist_ok=True)

board = pcbnew.BOARD()
board.SetCopperLayerCount(4)

# L1(Top)=high-speed signal, L4(Bottom)=low-speed signal -- default F.Cu/B.Cu
# names are fine for those, routing intent is a design-time choice not a
# file property. Name the two inner planes per the project's stackup spec
# (see pcb/README.md) so it's unambiguous in the layer picker.
board.SetLayerName(pcbnew.In1_Cu, "GND_Plane")
board.SetLayerName(pcbnew.In2_Cu, "Power_Planes")

# title block: bump REVISION/DATE here when the layout changes
REVISION, DATE = "2.4", "2026-10-03"
tb = board.GetTitleBlock()
tb.SetTitle("ESP32 PC Controller System")
tb.SetRevision(REVISION)
tb.SetDate(DATE)

pcbnew.SaveBoard(OUT, board)
print("wrote", OUT)

reloaded = pcbnew.LoadBoard(OUT)
print("copper layers:", reloaded.GetCopperLayerCount())
print("In1 name:", reloaded.GetLayerName(pcbnew.In1_Cu))
print("In2 name:", reloaded.GetLayerName(pcbnew.In2_Cu))
