# Power control and night mode

How the board turns the PC on and off, and the night mode it runs on its own.
The console side is in [console.md](console.md), the buttons in
[discord.md](discord.md#control-panel).

## Power control

| Action | Path |
|---|---|
| Power on from shutdown | Wake-on-LAN magic packet on the home network (`wake`) |
| Shutdown / sleep / hibernate / restart / lock | `power <sleep\|hibernate\|restart\|shutdown\|lock>`: the board sends `!power <action> <nonce>` over the USB link, the PC helper hands it to PcHwCtl, which accepts or declines at once and acts 1.5 s later. With no answer from the helper in 3 s, sleep and shutdown fall back to HID System Control keys |
| Hard reset of a hung PC | RST opto (`reset` command: this resets the PC, not the board) |
| Power cut | BIOS "Always Power On after AC loss" boots the PC when power returns |

What the PC needs for this:
- BIOS: Wake-on-LAN enabled, "Restore on AC power loss" = Power On. The
  ErP / deep-sleep option must be off, or the network card loses power at
  shutdown and WoL cannot work.
- Windows: Fast Startup off (it can stop WoL from working after a shutdown), and the network
  adapter's "Wake on Magic Packet" enabled. Updates sometimes reset these.
- Wired Ethernet: most WiFi adapters can't wake from shutdown.

If the ESP32 itself gets stuck, flipping the PSU switch power-cycles it, and
the AC-loss setting brings the PC back up. See
[recovery-and-physical-access.md](recovery-and-physical-access.md).

Every power action (sleep, hibernate, restart, shutdown, lock) is PcHwCtl's
job: the PC service only forwards it (`power do <action> 1500`) and acks the
board. PcHwCtl can decline ("Hibernation is off.", "Windows is installing
updates.", ...), and the board posts the reason to Discord. With PcHwCtl not
running the answer is "PcHwCtl offline, use Reset": nothing on the PC can act
then, and PC reset (behind its confirm) is what is left. Lock needs this path
too: the board has no keyboard to send Win+L.

## Night mode

**Night mode** (PC Control, console `night`) engages between 22:00 and 06:00
once the PC has had no keyboard or mouse input for 5 minutes (the service's
user-session half reads Windows' idle counter, stat `pc.idle_s`). It sets the
PC's RGB LEDs to 10 %, the board's panels to 10 % on their PC-off screens, and
caps the status LED at 20 %. It also turns the PC's displays off, and back on when
it ends (or interrupted): a PC asleep or hibernated then gets them on as soon as
it is back, retried every 30 s until the PC service says ok. Input on the
PC puts everything back, and 5 more idle minutes engage it again. At 06:00 the
board side goes back at once, and the LEDs go back as soon as the PC and
PcHwCtl answer. Each change is posted to #pc-announcement. "Night auto" is the
automatic engage; "Start night now" engages it at once, at any hour, until
input on the PC or "End night now".

Both night mode and Low power mode also switch Windows to PcHwCtl's "PcHwCtl
Night" power plan (5 % processor cap, half the cores parked; PcHwCtl
`docs/ipc.md`, "The PcHwCtl Night plan"), and back to PcHwCtl's day plan
(High performance) when they end. The PC Control tab shows the active plan.

## Low power mode

**Low power mode** (PC Control button "Low power", console `night low on|off`)
is the night's power side by hand, at any hour, until turned off: Process
Lasso's night config (E-cores in efficiency mode, heavy exes left alone, the
same `!night 1` night mode sends), PC LEDs 10 %, board panels 10 % with their
normal faces, status LED capped at 20 %. Displays stay on, the PC never sleeps,
and input does not end it. It survives a board restart. On and off are posted
to #pc-announcement and Pushover; a night that ends while it is on leaves it on.

## Guards against false triggers

- **Wake check.** Input during the night ends it at once: the PC's displays
  come back on, with everything else. If the next 10 s show no real return
  (more than 50 px of mouse travel, or 3 key / mouse button presses;
  `pc.in_px`, `pc.in_press` from the user-session half, gamepad buttons and
  sticks count too), a bumped mouse, the night comes straight back, with no
  post, and its sleep clock carries on. A real return keeps it off and posts
  "Back to normal".
- **Video.** No automatic start while a process keeps the display on (a video
  playing, a presentation): PcHwCtl reads `powercfg /requests` every 10 s and
  the PC service passes it on (`pc.disp_req`, `pc.disp_req_by`). The PC
  Control tab shows who holds it.

## The night pass on the PC

While engaged, the PC service has PcHwCtl move every non-critical
background process to four E-cores (14-17 on the Core Ultra 7 265K) in Process
Lasso's config (`C:\ProgramData\ProcessLasso\config\prolasso.ini`), pin the
running ones at once and put them in Windows' efficiency mode (`lasso save
day`, then `aff set` and `eco on` per exe; PcHwCtl puts new instances in
efficiency mode too). The kernel and session processes, the desktop shell,
audio, this stack, Process Lasso and its ProBalance exclusions are left alone.
So is a heavy exe: one averaging 0.8 cores or more (1.0 = one core busy,
summed over its instances) for 2 minutes, such as a render or a long
calculation, and the foreground full-screen app (a game can be light on CPU).
The check runs all night: a pinned exe that turns heavy gets its day rule back
and leaves efficiency mode, and an exe left alone is pinned once it stays
under 0.3 cores for 5 minutes. When the night ends, `lasso load day` puts the
normal config back and `eco off all` lifts efficiency mode. If the board stops
confirming the night for 5 minutes, the service does that by itself.

## Sleep and hibernate

Sleep (Windows' own timers off; the board keeps it, in the night window only):
1 hour after the last real input on the PC (false wakes don't count) the board
sleeps the PC. There is no timed hibernate. It waits, with the displays off,
while an exe is heavy (`pc.heavy`, kept ones included except
`HOST_NIGHT_HEAVY_IGNORE`: `svchost` and `msmpeng` by default, since a service
host summed over dozens of instances never calms down; they are still left
off the E-cores while heavy), a power request keeps the PC up (`pc.disp_req`,
`pc.sys_req`, `pc.exec_req`, minus `NIGHT_REQ_IGNORE`: `claude.exe` always
holds one) or the network averages more than 1 MB/s either way. Real work
still holds sleep through its power request, which the board checks on its
own.

Hibernate happens when you leave home: the phone (OwnTracks, the weather
phone engine's location) goes from within 1 km of home to beyond it
(`NIGHT_LEAVE_KM`), at any hour, with Night auto on. A PC night mode put to
sleep is woken and hibernated; a PC that is on is hibernated only if nobody
used it for 5 min (`NIGHT_LEAVE_IDLE_S`), else a post says it was left on. A PC
that is off for another reason is left alone. It needs a home (`weather home`)
and a valid phone fix (12 h, `OWNTRACKS_MAX_AGE_S`). A hibernated PC stays so until it is powered
on. When the PC comes back the board sends its night state at once,
so the day config is back right after power-on. Night mode posts its own
sleep, wake and hibernate messages; for 3.5 min after each of its requests the
generic ones ("wake PC", "PC accepted hibernate", "PC is asleep or off", "PC is
on") are left out of Discord and Pushover.

## Power-on brownout

Some motherboards drag the USB 5 V rail down while the PC powers on from
shutdown: the board browns out and restarts (`boot: last reset: brownout` in
the log). Every wake therefore turns the backlights off while the WoL / USB
wake goes out and for 200 ms after it (`POWER_WAKE_DIM_MS` in settings.h). It
used to be a switch; it is always on since the brownout came back on
2026-10-27 (the author suspects capacitance on the motherboard bleeding off).
Seen on the author's board, both with Wake PC and with the PC's own power
button; waking from sleep is not affected.

It does not cover a power-on from the PC's own power button, which never goes
through the board. The real fix is a supply that the PC's power-on cannot pull
down. The ESP32 board's second USB port is not that supply: its 5 V in and out
are bridged, so feeding 5 V there while the PC's USB also powers the board would
damage it. What would work is an 18650 cell with a 5 V boost converter feeding
the board, plus some way to charge the cell; not worth it on the author's setup,
but an option if your board browns out on every power-on.

The same sag also flipped bits in PSRAM; what the firmware does about that
(PSRAM at 40 MHz with ECC, a second restart after a brownout boot) is in
[firmware.md](firmware.md#psram-hardening).
