"""
Auto-routes pcb/layouts/default/default.kicad_pcb using Freerouting
(external, https://github.com/freerouting/freerouting -- the jar used
here is cached at K:\\cache\\freerouting\\freerouting-2.4.1.jar, not
committed to this repo, since neither official `kicad-cli` nor KiCad's
bundled `pcbnew` Python API include an autorouter -- KiCad dropped its
own built-in autorouter years ago).

Run AFTER layout_board.py and add_pours.py (needs final placement + the
GND/5V plane zones already poured -- those two nets end up fully
connected by the zone fill alone, since every part on this board is
through-hole and THT pad copper exists on every layer by default, so
there's nothing left for Freerouting to route on those nets).

IMPORTANT: only run this against a FRESH, UNROUTED board (straight out of
add_pours.py). Do not re-run it in place on a board this script already
routed -- tried that once: re-exporting an already-routed board back to
DSN and routing it again left 1 net unrouted (Freerouting partially
reused the existing traces as fixed obstacles and painted itself into a
corner), where routing fresh from an unrouted board hit 0 unrouted both
times. If the layout changes and the board needs rerouting, go back to
make_blank_pcb.py and rebuild the whole chain (blank -> sync -> layout ->
pours -> route) rather than routing on top of existing traces.

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\route_board.py

Steps: export Specctra DSN -> run Freerouting headless (F.Cu/B.Cu only;
GND_Plane/Power_Planes marked non-routable since they're solid pours,
not signal layers) -> import the routed Specctra SES back into the board
-> re-fill the copper zones.

That last re-fill matters and isn't a no-op: zones filled *before*
routing don't know about the vias routing adds afterward, so they won't
have carved clearance around them -- DRC catches this as a straight
zone/via clearance violation (seen in practice: 16 real errors right
after SES import, 0 after re-filling).
"""
import subprocess
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)
from board_config import (COPPER_LAYERS, POWER_NETS,
                          POWER_TRACK_W, POWER_VIA_D, POWER_VIA_DRILL)
import sys

import pcbnew

BOARD_DIR = os.path.join(ROOT, "pcb", "layouts", "default")
BOARD_PATH = BOARD_DIR + r"\default.kicad_pcb"
DSN_PATH = BOARD_DIR + r"\default.dsn"
SES_PATH = BOARD_DIR + r"\default.ses"
FREEROUTING_JAR = r"K:\cache\freerouting\freerouting-2.4.1.jar"



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


if COPPER_LAYERS == 4:
    # Layer order as written into the DSN (confirmed by inspection, not
    # assumed): F.Cu, GND_Plane, Power_Planes, B.Cu. Only the two real
    # signal layers are routable -- the plane layers are solid pours.
    freeroute("true,false,false,true", lock_result=False)
else:
    # 2-layer (rev 2.4b): B.Cu is the GND pour and may only carry short
    # jumpers. Freerouting ignores per-layer cost settings in the DSN (it
    # recomputes trace costs per board), so instead route in two passes:
    #   1. F.Cu only -- everything that can be routed without crossing.
    #      Lock the result.
    #   2. F.Cu + B.Cu -- only the leftover crossings remain, and with the
    #      pass-1 copper fixed they come out as via -> short B.Cu hop -> via.
    freeroute("true,false", lock_result=True)
    freeroute("true,true", lock_result=False)
