"""
Verifies the display-bus routing that route_spi.py lays down: every display
net (SCK, MOSI, DC, RST1 + CS1-3; RST2/3 are Freerouted) is F.Cu only, has no vias, has NO node
where 3+ track ends meet (a T-junction / star), and walks as a single chain
from the MCU pad through each display pad in order.

    "C:\Program Files\KiCad\10.0\bin\python.exe" Scripts\check_spi_topology.py

Run after route_board.py (or any manual edit in KiCad touching these nets).
Coordinates are compared at 1um resolution: pad centres on the 180-degree
display sockets land ~1nm off round mm values.
"""
import pcbnew
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)
from collections import Counter
mm=pcbnew.ToMM
b=pcbnew.LoadBoard(os.path.join(ROOT, "pcb", "layouts", "default", "default.kicad_pcb"))
fps={f.GetReference():f for f in b.GetFootprints()}
nets=["gpio12","disp_sck","gpio11","disp_mosi","disp_dc","disp_rst1"]+[fps[r].FindPadByNumber("7").GetNetname() for r in ("J3","J4","J5")]
for n in nets:
    tr=[t for t in b.GetTracks() if t.GetNetname()==n]
    deg=Counter()
    for t in tr:
        deg[(round(t.GetStart().x,-3),round(t.GetStart().y,-3))]+=1; deg[(round(t.GetEnd().x,-3),round(t.GetEnd().y,-3))]+=1
    pads={(round(p.GetPosition().x,-3),round(p.GetPosition().y,-3)):f"{p.GetParentFootprint().GetReference()}.{p.GetNumber()}" for p in b.GetPads() if p.GetNetname()==n}
    tees=[(round(mm(x),2),round(mm(y),2),d) for (x,y),d in deg.items() if d>=3]
    order=[]  # walk chain from the MCU pad
    start=[k for k,v in pads.items() if v.split(".")[0] not in ("J3","J4","J5")][0]  # MCU pad or its series resistor
    adj={}
    for t in tr:
        a=(round(t.GetStart().x,-3),round(t.GetStart().y,-3)); c=(round(t.GetEnd().x,-3),round(t.GetEnd().y,-3))
        adj.setdefault(a,[]).append(c); adj.setdefault(c,[]).append(a)
    seen={start}; cur=start; length=0
    while True:
        if cur in pads: order.append(pads[cur])
        nxt=[p for p in adj.get(cur,[]) if p not in seen]
        if not nxt: break
        length+=((nxt[0][0]-cur[0])**2+(nxt[0][1]-cur[1])**2)**0.5; seen.add(nxt[0]); cur=nxt[0]
    layers={b.GetLayerName(t.GetLayer()) for t in tr}
    vias=sum(1 for t in tr if t.GetClass()=="PCB_VIA")
    print(f"{n:10s} layers={layers} vias={vias} T/star nodes={tees or 'none'} chain={' -> '.join(order)} len={mm(length):.1f}mm")
