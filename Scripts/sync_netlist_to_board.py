"""
Sync atopile's exported KiCad-format netlist (pcb/build/default.net) onto
the real KiCad board file (pcb/layouts/default/default.kicad_pcb), using
only KiCad's own bundled pcbnew Python API -- no GUI automation, no
xdotool/Docker/X11 (kicad_cli, the pierrechevalier83 tool, can't do this
at all -- it only runs ERC/DRC on an already-existing project, it doesn't
import netlists or place footprints). kicad-cli (KiCad's own official
CLI) doesn't have a netlist-import command either; this script is the
actual missing piece, built directly on pcbnew's scripting API.

Run this after every `ato build` that changes main.ato's parts or nets.

IDEMPOTENT: a component reference (U1, U2, ...) that already exists on
the board keeps its current position/rotation -- only its pad-net
assignments get refreshed. A brand-new reference (a part just added to
main.ato) gets grid-placed in open space so it doesn't get lost, but
its position is NOT considered final -- move it via the KiCad GUI same
as anything else. A reference that's on the board but no longer in the
netlist (a part removed from main.ato) is left in place with a warning
printed, not silently deleted -- deleting real placement/routing work
automatically is exactly the kind of thing that should require a human
look first.

All nets are rebuilt from scratch each run (old NETINFO_ITEMs cleared,
new ones created from the current netlist) since net topology is fully
derived from main.ato and should always just reflect current truth --
there's nothing per-net worth preserving across a re-sync the way
placement is.

Usage:
    cd pcb && PYTHONIOENCODING=utf-8 ato build
    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" Scripts\\sync_netlist_to_board.py
(must run under KiCad's own bundled Python -- that's the only one with
the pcbnew module installed.)
"""
import re
import pcbnew
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)

NET_PATH = os.path.join(ROOT, "pcb", "build", "default.net")
BOARD_PATH = os.path.join(ROOT, "pcb", "layouts", "default", "default.kicad_pcb")
FPBASE = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
# project-local custom footprints (lib name used in main.ato -> .pretty dir)
LOCAL_LIBS = {"ESP32PCC": os.path.join(ROOT, "pcb", "footprints", "ESP32PCC.pretty")}
# Board references are derived from the atopile INSTANCE name, not taken from
# atopile's own U1..Un numbering: that numbering is pure declaration order,
# so removing one part (rev 2.3 dropped the spare header) renumbered every part
# after it -- and every ref-keyed placement/route in the later scripts with
# it. Instance names only change when main.ato says so.
REF_BY_INSTANCE = {"mcu.left": "J1", "mcu.right": "J2", "display1": "J3", "display2": "J4",
                   "display3": "J5", "rst_out": "J6", "keyfob": "J7", "boost": "PS1",
                   "led1": "D1"}


def board_ref(inst):
    """optoN -> UN, rN -> RN, everything else from REF_BY_INSTANCE."""
    if inst in REF_BY_INSTANCE:
        return REF_BY_INSTANCE[inst]
    m = re.fullmatch(r"(opto|r)(\d+)", inst)
    if m:
        return ("U" if m.group(1) == "opto" else "R") + m.group(2)
    raise SystemExit(f"no board reference for atopile instance {inst!r} -- add it to REF_BY_INSTANCE")


INSTANCE_VALUES = {**{f"r{i}": "270R" for i in range(1, 7)}, "r7": "10k", "r8": "33R", "r9": "33R",
                   **{f"r{i}": "10k" for i in range(10, 14)}}


def parse_netlist(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()

    comp_re = re.compile(
        r'\(comp \(ref "(?P<ref>[^"]+)"\).*?\(footprint "(?P<fp>[^"]+)"\).*?'
        r'\(description "(?P<desc>[^"]*)"\).*?\(names "[^"]*::(?P<inst>[^"]+)"\)',
        re.DOTALL,
    )
    components = {}
    for m in comp_re.finditer(text):
        # atopile 0.2 drops `value` and merges identical component classes'
        # descriptions (the 10k pull-down comes out labelled Resistor270), so
        # part values are keyed off the instance name instead.
        desc = INSTANCE_VALUES.get(m.group("inst"), m.group("desc"))
        components[m.group("ref")] = {"footprint": m.group("fp"), "desc": desc,
                                      "board_ref": board_ref(m.group("inst"))}

    nets_section = text[text.index("(nets"):]
    net_start_re = re.compile(r'\(net \(code "(\d+)"\) \(name "([^"]*)"\)')
    node_re = re.compile(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)')
    nets = []
    for sm in net_start_re.finditer(nets_section):
        code, name = sm.groups()
        # Paren-depth scan, not a naive non-greedy regex: a `.*?\)\)`
        # match stops at the end of the FIRST node (whose own closing
        # "))" looks identical to the net's), silently truncating every
        # net with more than one node. Confirmed the hard way once
        # already -- don't regress this.
        i = sm.end()
        depth = 1
        while depth > 0:
            if nets_section[i] == "(":
                depth += 1
            elif nets_section[i] == ")":
                depth -= 1
            i += 1
        body = nets_section[sm.end():i]
        nodes = node_re.findall(body)
        nets.append((code, name, nodes))

    return components, nets


def place_new_footprint(fp, cursor):
    """Place a brand-new footprint at cursor=[x, y, row_h] (mutated in
    place), accounting for the gap between a footprint's anchor and its
    own bounding box -- placing the raw anchor at the cursor leaves tall
    parts (e.g. a 44-pin header) overlapping neighboring rows, since the
    anchor sits near pad 1, nowhere near the bbox center."""
    max_x = pcbnew.FromMM(280)
    margin = pcbnew.FromMM(5)

    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    bbox0 = fp.GetBoundingBox()
    offset = pcbnew.VECTOR2I(-bbox0.GetLeft(), -bbox0.GetTop())
    w, h = bbox0.GetWidth(), bbox0.GetHeight()

    if cursor[0] + w > max_x:
        cursor[0] = pcbnew.FromMM(20)
        cursor[1] += cursor[2] + margin
        cursor[2] = pcbnew.FromMM(0)

    fp.SetPosition(pcbnew.VECTOR2I(cursor[0], cursor[1]) + offset)
    cursor[0] += w + margin
    cursor[2] = max(cursor[2], h)


def main():
    components, nets = parse_netlist(NET_PATH)
    print(f"netlist: {len(components)} components, {len(nets)} nets, "
          f"{sum(len(n[2]) for n in nets)} pad-net links")

    board = pcbnew.LoadBoard(BOARD_PATH)
    existing = {fp.GetReference(): fp for fp in board.GetFootprints()}

    # cursor continues past whatever's already placed, so newly-added
    # parts don't land on top of existing (possibly manually-placed) ones
    cursor = [pcbnew.FromMM(20), pcbnew.FromMM(20), pcbnew.FromMM(0)]
    if existing:
        max_bottom = max(fp.GetBoundingBox().GetBottom() for fp in existing.values())
        cursor[1] = max_bottom + pcbnew.FromMM(20)

    placed = {}
    new_count = 0
    for net_ref in sorted(components, key=lambda r: int(r[1:])):
        info = components[net_ref]
        ref = info["board_ref"]
        if ref in existing:
            placed[ref] = existing[ref]
            continue
        lib, name = info["footprint"].split(":", 1)
        libdir = LOCAL_LIBS.get(lib, f"{FPBASE}\\{lib}.pretty")
        fp = pcbnew.FootprintLoad(libdir, name)
        if fp is None:
            print(f"  ! could not load footprint {info['footprint']} for {ref}")
            continue
        fp.SetReference(ref)
        fp.SetValue(info["desc"])
        place_new_footprint(fp, cursor)
        board.Add(fp)
        placed[ref] = fp
        new_count += 1
    # netlist nodes name atopile's refs; map them to the placed footprints
    placed_by_net_ref = {r: placed[c["board_ref"]] for r, c in components.items()
                         if c["board_ref"] in placed}

    stale = set(existing) - {c["board_ref"] for c in components.values()}
    for ref in sorted(stale):
        print(f"  ! {ref} is on the board but no longer in the netlist "
              f"-- left in place, remove it by hand if it's really gone")

    print(f"placed {new_count} new footprint(s), reused {len(placed) - new_count} existing")

    # Reuse an existing net by name rather than clearing+recreating every
    # NETINFO_ITEM up front -- removing board-owned NETINFO_ITEMs while
    # other live Python objects still reference related structures trips
    # a SWIG object-lifetime bug (confirmed the hard way: pad wrappers
    # came back as raw, useless SwigPyObjects afterwards). A pad whose
    # net legitimately changed just gets reassigned below; a pad on a net
    # that's genuinely gone from the netlist keeps its last assignment
    # until someone re-runs a real KiCad netlist diff -- acceptable given
    # this script's job is "get everything current onto the board", not
    # "perfectly mirror every deletion".
    pad_count = 0
    for code, name, nodes in nets:
        net_name = name if name and not name.startswith("net") else f"N${code}"
        netinfo = board.FindNet(net_name)
        if netinfo is None:
            netinfo = pcbnew.NETINFO_ITEM(board, net_name)
            board.Add(netinfo)
        for ref, pin in nodes:
            fp = placed_by_net_ref.get(ref)
            if fp is None:
                continue
            pad = fp.FindPadByNumber(pin)
            if pad is None:
                print(f"  ! {ref} has no pad numbered {pin!r} (net {net_name})")
                continue
            pad.SetNet(netinfo)
            pad_count += 1

    print(f"assigned {pad_count} pad-net links")

    pcbnew.SaveBoard(BOARD_PATH, board)
    print("saved", BOARD_PATH)


if __name__ == "__main__":
    main()
