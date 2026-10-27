"""
Checks that the firmware pin map (the PIN_* settings) matches the
netlist (pcb/main.ato): for every PIN_* the GPIO the firmware drives must be
the one wired to that signal's part pin, directly or through one series
resistor (the 33R on SCK/MOSI, the 270R in front of each opto and the LED).
The pins are read the way the build sees them: settings.h when it exists
(else settings_example.h), defaults from settings_or_example.h.

Plain Python, no KiCad needed:

    python scripts/check_pins.py

Exit code 1 and a list of mismatches if anything disagrees.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATO = os.path.join(ROOT, "pcb", "main.ato")
MAIN = os.path.join(ROOT, "firmware", "main")
DEFAULTS = os.path.join(MAIN, "settings_or_example.h")
SETTINGS = os.path.join(MAIN, "settings.h")
if not os.path.exists(SETTINGS):
    SETTINGS = os.path.join(MAIN, "settings_example.h")

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
    pins = {}
    for path in (DEFAULTS, SETTINGS):   # settings.h wins over the defaults
        text = open(path, encoding="utf-8").read()
        pins.update({name: int(v) for name, v in re.findall(r"#define\s+(PIN_\w+)\s+(\d+)\b", text)})
        for name, vals in re.findall(r"#define\s+(PIN_\w+)_LIST\s+\{([^}]*)\}", text):
            pins[name] = [int(v) for v in vals.split(",")]
    return pins


def main():
    net_of = ato_nets()
    fw = firmware_pins()
    errors = []
    for name, want in EXPECTED.items():
        if name not in fw:
            errors.append(f"{name}: not defined in the settings")
            continue
        targets = want if isinstance(want, list) else [want]
        values = fw[name] if isinstance(fw[name], list) else [fw[name]]
        if len(values) != len(targets):
            errors.append(f"{name}: {len(values)} pins in the settings, {len(targets)} on the board")
            continue
        for gpio, target in zip(values, targets):
            board = gpio_for(net_of, target)
            if board != {gpio}:
                errors.append(f"{name}: firmware GPIO{gpio}, board wires {target} to "
                              + (", ".join(f"GPIO{g}" for g in sorted(board)) or "no GPIO"))
    for name in sorted(set(fw) - set(EXPECTED)):
        errors.append(f"{name}: in the settings but not checked here -- add it to EXPECTED")
    if errors:
        print(f"pin map mismatch ({os.path.relpath(SETTINGS, ROOT)} vs pcb/main.ato):")
        for e in errors:
            print("  " + e)
        sys.exit(1)
    print(f"pin map OK: {len(EXPECTED)} signals match pcb/main.ato")


if __name__ == "__main__":
    main()
