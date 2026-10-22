"""
Removes every right-angle (and sharper) track corner: each corner where two
segments of the same net meet at an interior angle under MAX_INTERIOR_DEG is
cut back by CHAMFER mm on both legs and joined with a 45-degree diagonal.

Run AFTER route_board.py (works on both the locked hand-routed display bus
and Freerouting's tracks):

    <kicad-python> scripts/chamfer_corners.py

Each new diagonal is collision-checked (pcbnew shape Collide) against every
other-net pad / track / via on its layer at CLEARANCE; if it would collide,
the chamfer is halved down to MIN_CHAMFER, else the corner is left and
reported. Corners sitting on a pad or via are skipped (they're inside copper
anyway). Sub-10um segments are merged into their neighbours first. Nested bus lanes stay parallel with equal chamfers: at a 0.5mm
lane pitch the diagonals end up 0.71mm apart, more than the straight runs.
"""
import math
from collections import defaultdict

import pcbnew

from board_config import BOARD_PATH
MM = pcbnew.FromMM
CHAMFER = 0.5
MIN_CHAMFER = 0.12
CLEARANCE = 0.21          # netclass clearance 0.2 + margin
MAX_INTERIOR_DEG = 120    # chamfer 90-degree (and sharper) corners; leave 135s

board = pcbnew.LoadBoard(BOARD_PATH)
tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
pads = list(board.GetPads())


MICRO = MM(0.01)  # Freerouting leaves the odd nm-long segment at a bend


def seglen(t):
    return (t.GetEnd() - t.GetStart()).EuclideanNorm()


def key(layer, p):
    return (layer, p.x, p.y)


ends = defaultdict(list)
for t in tracks:
    ends[key(t.GetLayer(), t.GetStart())].append(t)
    ends[key(t.GetLayer(), t.GetEnd())].append(t)


def on_pad_or_via(layer, pt):
    for v in vias:
        if v.GetPosition() == pt:
            return True
    for p in pads:
        if p.IsOnLayer(layer) and p.HitTest(pt):
            return True
    return False


def other_end(t, pt):
    return t.GetEnd() if t.GetStart() == pt else t.GetStart()


def collides(seg, layer, net):
    shape = seg.GetEffectiveShape(layer)
    clr = MM(CLEARANCE)
    for p in pads:
        if p.GetNetCode() != net and p.IsOnLayer(layer) and shape.Collide(p.GetEffectiveShape(layer), clr):
            return True
    for o in tracks + vias:
        if o.GetNetCode() != net and o.IsOnLayer(layer) and shape.Collide(o.GetEffectiveShape(layer), clr):
            return True
    return False


done = skipped = 0
for (layer, x, y), pair in list(ends.items()):
    if len(pair) != 2:
        continue
    a, b = pair
    if a.GetNetCode() != b.GetNetCode():
        continue
    P = pcbnew.VECTOR2I(x, y)
    A, B = other_end(a, P), other_end(b, P)
    ua = (A.x - x, A.y - y)
    ub = (B.x - x, B.y - y)
    la, lb = math.hypot(*ua), math.hypot(*ub)
    if la < MICRO or lb < MICRO:
        continue  # a micro-segment corner: handled in the second pass
    cosang = (ua[0] * ub[0] + ua[1] * ub[1]) / (la * lb)
    if math.degrees(math.acos(max(-1.0, min(1.0, cosang)))) >= MAX_INTERIOR_DEG:
        continue
    if on_pad_or_via(layer, P):
        continue
    c = min(MM(CHAMFER), 0.45 * la, 0.45 * lb)
    placed = False
    while c >= MM(MIN_CHAMFER):
        P1 = pcbnew.VECTOR2I(int(x + ua[0] / la * c), int(y + ua[1] / la * c))
        P2 = pcbnew.VECTOR2I(int(x + ub[0] / lb * c), int(y + ub[1] / lb * c))
        diag = pcbnew.PCB_TRACK(board)
        diag.SetStart(P1)
        diag.SetEnd(P2)
        diag.SetWidth(min(a.GetWidth(), b.GetWidth()))
        diag.SetLayer(layer)
        diag.SetNet(a.GetNet())
        if not collides(diag, layer, a.GetNetCode()):
            for t, Pn in ((a, P1), (b, P2)):
                if t.GetStart() == P:
                    t.SetStart(Pn)
                else:
                    t.SetEnd(Pn)
            diag.SetLocked(a.IsLocked() and b.IsLocked())
            board.Add(diag)
            tracks.append(diag)
            placed = True
            break
        c /= 2
    if placed:
        done += 1
    else:
        skipped += 1
        print(f"  ! left a corner at ({pcbnew.ToMM(x):.2f},{pcbnew.ToMM(y):.2f}) "
              f"on {board.GetLayerName(layer)} ({a.GetNetname()}) -- no room to chamfer")

# ---- micro-segment corners: a bend hidden behind a nm-long segment m. The
# legs a/b end at m's two ends; reuse m itself as the diagonal (moving rather
# than deleting tracks -- board.Remove() leaves stale SWIG wrappers).
for m in [t for t in tracks if seglen(t) < MICRO]:
    legs = []
    for q in (m.GetStart(), m.GetEnd()):
        near = [t for t in ends[key(m.GetLayer(), q)] if t is not m and seglen(t) >= MICRO]
        legs.append((q, near))
    if any(len(n) != 1 for _, n in legs):
        continue
    (qa, (a,)), (qb, (b,)) = legs
    A, B = other_end(a, qa), other_end(b, qb)
    ua, ub = (A.x - qa.x, A.y - qa.y), (B.x - qb.x, B.y - qb.y)
    la, lb = math.hypot(*ua), math.hypot(*ub)
    cosang = (ua[0] * ub[0] + ua[1] * ub[1]) / (la * lb)
    if math.degrees(math.acos(max(-1.0, min(1.0, cosang)))) >= MAX_INTERIOR_DEG:
        continue
    layer = m.GetLayer()
    c = min(MM(CHAMFER), 0.45 * la, 0.45 * lb)
    while c >= MM(MIN_CHAMFER):
        P1 = pcbnew.VECTOR2I(int(qa.x + ua[0] / la * c), int(qa.y + ua[1] / la * c))
        P2 = pcbnew.VECTOR2I(int(qb.x + ub[0] / lb * c), int(qb.y + ub[1] / lb * c))
        probe = pcbnew.PCB_TRACK(board)
        probe.SetStart(P1)
        probe.SetEnd(P2)
        probe.SetWidth(m.GetWidth())
        probe.SetLayer(layer)
        if not collides(probe, layer, m.GetNetCode()):
            for t, q, Pn in ((a, qa, P1), (b, qb, P2)):
                if t.GetStart() == q:
                    t.SetStart(Pn)
                else:
                    t.SetEnd(Pn)
            m.SetStart(P1)
            m.SetEnd(P2)
            done += 1
            break
        c /= 2
    else:
        skipped += 1
        print(f"  ! left a corner at ({pcbnew.ToMM(qa.x):.2f},{pcbnew.ToMM(qa.y):.2f}) -- no room")

filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
board.Save(BOARD_PATH)
print(f"chamfered {done} corners, {skipped} left")
