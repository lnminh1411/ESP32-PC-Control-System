"""
Auto-routes whatever route_spi.py left unrouted, with Freerouting
(https://github.com/freerouting/freerouting, jar at board_config.FREEROUTING_JAR,
not committed -- neither kicad-cli nor pcbnew include an autorouter). Since
rev 3 route_spi.py routes every net, so this is a safety net: Freerouting
may still print a few phantom "unrouted" items -- trust KiCad's DRC.

Run AFTER add_pours.py and route_spi.py.

IMPORTANT: only run this against a FRESH board (straight out of the chain).
Re-running it on a board it already routed left a net unrouted (Freerouting
reused the existing traces as fixed obstacles and painted itself into a
corner). To reroute after a layout change, rebuild the whole chain from
make_blank_pcb.py.

    <kicad-python> scripts/route_board.py

Steps: export Specctra DSN -> Freerouting headless -> import the routed SES
-> re-fill the copper zones. The re-fill matters: zones filled before routing
don't know about the vias routing adds, which DRC flags as zone/via
clearance violations until re-filled.
"""
import subprocess
import os
from board_config import (BOARD_DIR, BOARD_PATH, FREEROUTING_JAR, POWER_NETS,
                          POWER_TRACK_W, POWER_VIA_D, POWER_VIA_DRILL)
import sys

import pcbnew

DSN_PATH = os.path.join(BOARD_DIR, "default.dsn")
SES_PATH = os.path.join(BOARD_DIR, "default.ses")



def load_with_power_class():
    board = pcbnew.LoadBoard(BOARD_PATH)
    # Power net class: a script-loaded board doesn't read the .kicad_pro, so
    # the class (same values as the project file, which DRC/the GUI use) is
    # applied here for the DSN export -- Freerouting then routes 5V/12V at
    # full width.
    ns = board.GetDesignSettings().m_NetSettings
    nc = pcbnew.NETCLASS("Power")
    nc.SetTrackWidth(pcbnew.FromMM(POWER_TRACK_W))
    nc.SetClearance(pcbnew.FromMM(0.2))
    nc.SetViaDiameter(pcbnew.FromMM(POWER_VIA_D))
    nc.SetViaDrill(pcbnew.FromMM(POWER_VIA_DRILL))
    ns.SetNetclass("Power", nc)
    for net in POWER_NETS:
        ns.SetNetclassPatternAssignment(net, "Power")
    board.SynchronizeNetsAndNetClasses(True)
    return board


def freeroute(routable, lock_result):
    """export DSN -> Freerouting -> import SES -> refill; optionally lock
    everything routed so a later pass treats it as fixed."""
    board = load_with_power_class()
    if not pcbnew.ExportSpecctraDSN(board, DSN_PATH):
        sys.exit("DSN export failed")
    print("exported", DSN_PATH, "routable layers:", routable)
    result = subprocess.run(
        [
            "java", "-jar", FREEROUTING_JAR,
            "-de", DSN_PATH, "-do", SES_PATH,
            "--gui.enabled=false",
            f"--router.layers.routable={routable}",
            "-mp", "120",
        ],
        cwd=BOARD_DIR,
    )
    if result.returncode != 0:
        sys.exit(f"Freerouting failed with exit code {result.returncode}")
    if not pcbnew.ImportSpecctraSES(board, SES_PATH):  # keeps locked tracks
        sys.exit("SES import failed")
    print("imported", SES_PATH)
    if lock_result:
        for t in board.GetTracks():
            t.SetLocked(True)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())  # re-fill so zones carve clearance around the new vias
    board.Save(BOARD_PATH)
    print("saved", BOARD_PATH)


# B.Cu is the GND pour and may only carry short jumpers. Freerouting ignores
# per-layer cost settings in the DSN (it recomputes trace costs per board),
# so route in two passes:
#   1. F.Cu only -- everything that can be routed without crossing.
#      Lock the result.
#   2. F.Cu + B.Cu -- only the leftover crossings remain, and with the
#      pass-1 copper fixed they come out as via -> short B.Cu hop -> via.
freeroute("true,false", lock_result=True)
freeroute("true,true", lock_result=False)
