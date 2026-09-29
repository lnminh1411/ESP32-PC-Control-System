# Recovery & Physical Access — Why This Matters Before the PCB Goes In

Once the carrier PCB is soldered up and mounted inside the case, wired to the
motherboard's internal USB header and a power rail, there is **no physical
access** to the SuperMini's BOOT/RESET pins anymore — no popping the case to
mash a button on a whim. Whatever recovery paths exist have to already be
designed in before that happens. This doc exists because of what the 2026-09-29
bring-up session on the bench actually found, not theoretical worry.

## What happened on the bench (2026-09-29)

Bringing up the SuperMini + display test wiring on the bench surfaced two real
problems, not hypothetical ones:

1. **This SuperMini's RESET button is physically dead.** Only BOOT still
   works. (Cause unconfirmed — could be a bad solder joint, a cracked button,
   or defective from the factory. Didn't investigate further since BOOT alone
   was enough to work around it.)
2. **The chip got stuck re-entering the ROM bootloader on every reset**,
   including software-issued ones (`esptool --before usb-reset`, plain
   `chip-id` calls) — even with BOOT confirmed not physically stuck. Every
   soft-reset attempt landed back in the ROM loader instead of booting the
   flashed app. The only thing that cleared it was a **full USB power cycle**
   (unplug the cable, wait, replug) — not a soft reset, not BOOT+RESET, an
   actual power-on-reset.

Root cause, best guess: the S3's built-in USB-Serial/JTAG peripheral has an
internal "force download mode" latch that can get set by a tool session
(esptool, in this case) and only clears on true power-on-reset, not on the
soft resets that same peripheral can also issue. This most likely came from
*our own* repeated esptool probing during bring-up, not something that
happens organically from a single normal flash-and-walk-away — a plain
unplug/replug fixed it immediately, and did so as a first try. So the
practical odds of this happening from ordinary use (flash once, run) are low.
The structural problem it exposes is the real point:

**RESET is dead, and BOOT is now the only manual way to force this chip into
a recoverable state — full stop.** Once the board's in the case, that's gone.

## Why this stops being "just a bench inconvenience"

Right now, on the bench, "unplug and replug" is a two-second fix requiring no
tools. Once installed:

- The S3 is powered through the carrier PCB from an internal USB header, not
  a cable you can just unplug.
- **Many motherboards keep +5V standby power live on internal USB headers even
  when the PC is fully shut down** (this is what enables wake-on-USB/keyboard
  features). If that's true of the target motherboard, "shutting down the PC"
  may not actually power-cycle the S3 at all — the only real power cycle
  might require flipping the PSU's own rear switch or pulling the wall plug,
  which may not even be reachable without opening the case depending on the
  build.
- BOOT and RESET, whichever survives, have zero external access once the
  enclosure (`../cad/part1_esp32_cradle.scad`) is closed up and the PCB is
  screwed into the case.

If firmware ever ends up in a state that needs BOOT held during a reset to
recover — a bad OTA image that also corrupts the rollback partition, a
bricked bootloader from a failed flash, or this same stuck-latch condition
recurring for some other reason — there is currently **no path back** short
of disassembling the case and getting a probe back onto the header.

## What the existing architecture already covers (and doesn't)

The planned dual-OTA-partition rollback + 5s cross-ping watchdog (see prior
architecture discussion) is a real mitigation, but only for a specific class
of failure: **an app that boots and runs, just runs badly.** It does nothing
for:
- A chip stuck in the ROM bootloader (the ROM doesn't run any of that logic —
  it's not "the app," it's a lower layer underneath the app entirely).
- A corrupted bootloader partition itself.
- Any failure mode where the app never gets far enough to execute the
  watchdog/rollback code in the first place.

Software recovery and hardware recovery are different layers. The OTA
rollback plan covers the software layer. Nothing currently covers the
hardware layer once the case is closed.

## Decision (2026-09-29): no hardware change, procedural recovery instead

Considered and declined: breaking out BOOT/EN to an accessible header, and a
GPIO-controlled load switch on the S3's power feed (DevKit-triggered). Kept
`main.ato` as-is — no new parts, no new GPIO allocation.

Recovery plan going forward is two-tier, matching the two failure classes
actually observed on the bench:

1. **Ordinary firmware/OTA problems (the common case):** software reset via
   `esptool` from a host machine, over the S3's existing USB-C link to the
   motherboard. No physical access needed — this worked reliably every time
   during bring-up once the board was in a normal state; the one failure was
   the stuck-latch edge case below, not a general problem with software
   reset.
2. **Stuck-in-bootloader latch (the rare case — only actually hit it once,
   from repeated dev-tool probing):** shut the PC down, flip the PSU's own
   power switch off, wait, switch back on. A true power-on-reset is the one
   thing confirmed to clear this on the bench (plain unplug/replug fixed it
   first try). The PSU switch is external to the case on effectively every
   ATX build — no disassembly needed, unlike BOOT/EN which are inside a
   closed case either way.

This accepts that "the PC won't fully power down on its own" (standby rail
staying live) is expected and fine — the PSU switch bypasses that
deliberately, same as it would for any other reason to fully kill standby
power. Nothing to build for this; it's an operating procedure, not a PCB
feature. Worth writing into whatever end-user doc eventually covers "the
screen's frozen, what do I do" — the answer is PSU switch, not a support
ticket.
