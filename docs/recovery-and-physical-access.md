# Recovery and physical access

Once the carrier is mounted in the case there is no physical access to the
module's BOOT and RESET buttons. Whatever recovery exists has to work from
outside the case.

## What the bench showed (2026-09-29)

During bring-up the S3 got stuck re-entering the ROM bootloader on every
reset, including software resets from `esptool`, with BOOT not held. Only a
**full power cycle** (unplug, wait, replug) cleared it. Best guess: the
S3's USB-Serial/JTAG peripheral has a "force download" latch that a tool
session can set and that only a true power-on reset clears. It most likely
came from repeated `esptool` probing during bring-up, not from a normal
flash, and it happened once.

## Why it matters inside the case

- The module is powered from the PC's internal USB header, not a cable you
  can pull.
- Many motherboards keep +5V standby on internal USB headers when the PC is
  shut down (that is what wake-on-USB uses), so shutting the PC down may not
  power-cycle the ESP32 at all.
- An app-level watchdog only covers an app that boots and
  runs badly. It does nothing for a chip stuck in the ROM bootloader or a
  corrupt bootloader partition.

## Decision (2026-09-29): no hardware change, a procedure instead

Breaking BOOT/EN out to a header and a GPIO-controlled load switch on the
module's power were both considered and declined. Recovery is two-tier:

1. **Ordinary firmware problems (the common case):** reflash or soft-reset
   over the module's USB link from the PC. No physical access needed.
2. **Stuck in the bootloader (rare):** shut the PC down, switch the PSU off
   at its rear switch, wait, switch it back on. That is a true power-on
   reset for the ESP32, and with the BIOS set to "Always Power On after AC
   loss" the PC boots back up by itself. The PSU switch is outside the case
   on effectively every ATX build.

The standby rail staying live is expected and fine; the PSU switch is the
deliberate way to kill it. This belongs in any end-user note on "the screen
is frozen, what do I do".
