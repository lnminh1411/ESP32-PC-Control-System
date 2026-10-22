"""
Checks that the firmware pin map (firmware/carrier_main/config.h) matches the
netlist (pcb/main.ato): for every PIN_* the GPIO the firmware drives must be
the one wired to that signal's part pin, directly or through one series
resistor (the 33R on SCK/MOSI, the 270R in front of each opto and the LED).

Plain Python, no KiCad needed:

    python scripts/check_pins.py

Exit code 1 and a list of mismatches if anything disagrees.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATO = os.path.join(ROOT, "pcb", "main.ato")
CONFIG = os.path.join(ROOT, "firmware", "carrier_main", "config.h")

# firmware pin -> the part pin it has to reach on the board
EXPECTED = {
    "PIN_SCK": "display1.sck",
    "PIN_MOSI": "display1.mosi",
    "PIN_DC": "display1.dc",
    "PIN_CS": ["display1.cs", "display2.cs", "display3.cs"],
    "PIN_RST": ["display1.rst", "display2.rst", "display3.rst"],
    "PIN_BL": ["display1.bl", "display2.bl", "display3.bl"],
    "PIN_DOOR": ["opto1.anode", "opto2.anode", "opto3.anode", "opto4.anode"],
    "PIN_MOBO_RST": "opto5.anode",
    "PIN_LED": "led1.a",
}


def ato_nets():
    """Union-find over every `a ~ b` connection in the module body."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for line in open(ATO, encoding="utf-8"):
        line = line.split("#")[0].strip()
        m = re.fullmatch(r"([\w.]+)\s*~\s*([\w.]+)", line)
        if m and not line.startswith("signal"):
            parent[find(m[1])] = find(m[2])
    nets = {}
    for node in list(parent):
        nets.setdefault(find(node), set()).add(node)
    return {node: nets[find(node)] for node in parent}


def gpio_for(net_of, pin):
    """GPIO numbers wired to pin, directly or across one series resistor."""
    net = net_of.get(pin, {pin})
    gpios = {int(n[len("mcu.gpio"):]) for n in net if n.startswith("mcu.gpio")}
    if gpios:
        return gpios
    for node in net:
        m = re.fullmatch(r"(r\d+)\.p([12])", node)
        if m:
            far = f"{m[1]}.p{3 - int(m[2])}"
            gpios |= {int(n[len("mcu.gpio"):]) for n in net_of.get(far, ()) if n.startswith("mcu.gpio")}
    return gpios


def firmware_pins():
    text = open(CONFIG, encoding="utf-8").read()
    pins = {name: int(v) for name, v in re.findall(r"#define\s+(PIN_\w+)\s+(\d+)", text)}
    for name, vals in re.findall(r"(PIN_\w+)\[\d+\]\s*=\s*\{([^}]*)\}", text):
        pins[name] = [int(v) for v in vals.split(",")]
    return pins


def main():
    net_of = ato_nets()
    fw = firmware_pins()
    errors = []
    for name, want in EXPECTED.items():
        if name not in fw:
            errors.append(f"{name}: not defined in config.h")
            continue
        targets = want if isinstance(want, list) else [want]
        values = fw[name] if isinstance(fw[name], list) else [fw[name]]
        if len(values) != len(targets):
            errors.append(f"{name}: {len(values)} pins in config.h, {len(targets)} on the board")
            continue
        for gpio, target in zip(values, targets):
            board = gpio_for(net_of, target)
            if board != {gpio}:
                errors.append(f"{name}: firmware GPIO{gpio}, board wires {target} to "
                              + (", ".join(f"GPIO{g}" for g in sorted(board)) or "no GPIO"))
    for name in sorted(set(fw) - set(EXPECTED)):
        errors.append(f"{name}: in config.h but not checked here -- add it to EXPECTED")
    if errors:
        print("pin map mismatch (firmware/carrier_main/config.h vs pcb/main.ato):")
        for e in errors:
            print("  " + e)
        sys.exit(1)
    print(f"pin map OK: {len(EXPECTED)} signals match pcb/main.ato")


if __name__ == "__main__":
    main()
