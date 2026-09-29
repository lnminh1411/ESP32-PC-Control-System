# Carrier PCB (atopile)

Single-file atopile project: [`main.ato`](main.ato). Deliberately has zero external imports (no stdlib `Resistor`/`LED` etc.) — every part is defined locally with explicit pins + footprint, so it doesn't depend on guessing the right stdlib import path for whatever atopile version you have installed.

**Builds clean** on atopile 0.2.69 (`ato build`) — verified, netlist inspected node-by-node, every intended net (GND fan-out, 5V/VIN/boost power, UART cross-link, EN line, SPI, opto bank, 12V+ bus, both spare headers) came out exactly as designed. The only warnings are cosmetic "No MPN" notices — expected, since these are generic connectors/passives with no specific manufacturer part picked yet; doesn't block the netlist or footprint placement.

One gotcha if you regenerate `ato.yaml` from scratch: the version key is **`ato-version`** (hyphenated), not `ato_version` — the underscore form fails with an opaque `member 'ato_version' of 'ProjectConfig' objects is not valid SemVer string` error that has nothing to do with the actual version string.

## Build

```bash
cd pcb
ato build
```

This should produce a KiCad project you can open to place/route. **Board outline is not defined** — draw that in KiCad once the enclosure (`../cad/`) dimensions are locked in, since the outline depends on which CAD part this board actually lives inside.

## What's on it

| Connector | Pins | Footprint |
|---|---|---|
| SuperMini socket | 18 (2x9) | female, SuperMini plugs in with its own male DuPont pins |
| DevKit V1 socket | 30 (2x15) | female, same idea |
| Boost converter socket | 4 (1x4) | female, MT3608 module plugs in |
| Display socket | 8 (1x8) | female, near board edge |
| PC817 bank | 5x DIP-4 | real `Package_DIP:DIP-4_W7.62mm`, direct-solder |
| Status LEDs | 2x2, spaced apart | 5mm THT |
| Resistors | 7x2 | THT axial, 270Ω per the built circuit |
| Motherboard RST pair | 1x2 | isolated, male header |
| SuperMini spare header | 1x5 | male, GPIO9/10/11/12 + 1 unlabeled power pin |
| DevKit spare header | 2x9 | male, 18 confirmed-free GPIOs incl. a 3V3 tap |

## Three things to verify before ordering

1. **SuperMini's power pin feeding DevKit VIN + the boost converter is ambiguous** — the reference photo's silkscreen read as "BAT" on a closer look, not "5V". If it's genuinely the battery-charge pad rather than a clean 5V rail, DevKit's onboard regulator will be undervolted. One-line net change in `main.ato` if it needs moving to a real 5V source — check with a multimeter against USB-C VBUS once the board's in hand.
2. ~~SuperMini GPIO7 status-LED ambiguity~~ — **resolved 2026-09-29.** Bench-tested the real display against a real S3: its SCK line landed on GPIO7. `led2` (the S3-side status LED) moved to GPIO8 instead. GPIO8 wasn't part of the original 18-pin/2x9 socket definition — the footprint is now 20-pin/2x10 to fit it as a 19th signal. **Still needs verifying:** does the real SuperMini header actually have 20 pins, or 18? If 18, GPIO8 doesn't physically exist there and `pwr_spare` (currently read as an unconnected free pin) is the most likely candidate to actually be GPIO8 mislabeled. See [`../docs/recovery-and-physical-access.md`](../docs/recovery-and-physical-access.md) for why GPIO0/BOOT access matters just as much as this.
3. **Exact GPIO→door-button/RST row assignment** (which of DevKit D18/D21/D19/D22/D4 is UP vs DOWN vs RST) was reconstructed from a busy hand-drawn diagram and is unconfirmed. Doesn't affect this board's copper — each row is independent regardless of label — but firmware needs the real mapping from Cirkit Designer before flashing.

No hardware interlock exists on this board for the door UP/DOWN optocouplers — that 500ms/no-simultaneous rule from the original brief has to live in firmware, same as before.
