"""
Create a blank 2-layer KiCad board file at pcb/layouts/default/default.kicad_pcb.

WARNING: overwrites that file unconditionally, discarding any placement/
routing work already done. Only run this once, before the board exists,
or deliberately to start the layout over. After real placement/routing
has begun, do NOT re-run this -- use sync_netlist_to_board.py instead,
which only re-syncs nets onto an EXISTING board's already-placed
footprints (see its own docstring for how it handles added/removed
parts).

Usage:
    <kicad-python> scripts/make_blank_pcb.py
(must run under KiCad's own bundled Python -- that's the only one with
the pcbnew module installed.)
"""
import pcbnew
import os
from board_config import BOARD_PATH as OUT, REVISION, DATE, TITLE

os.makedirs(os.path.dirname(OUT), exist_ok=True)

board = pcbnew.BOARD()
board.SetCopperLayerCount(2)

# title block: bump REVISION/DATE in board_config.py when the layout changes
tb = board.GetTitleBlock()
tb.SetTitle(TITLE)
tb.SetRevision(REVISION)
tb.SetDate(DATE)

pcbnew.SaveBoard(OUT, board)
print("wrote", OUT)

reloaded = pcbnew.LoadBoard(OUT)
print("copper layers:", reloaded.GetCopperLayerCount())
print("revision:", reloaded.GetTitleBlock().GetRevision())
