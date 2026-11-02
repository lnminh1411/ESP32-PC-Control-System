# PC power-state detection over USB

The ESP32-S3's native USB-OTG peripheral, run by TinyUSB (esp_tinyusb),
reports the host bus state as TinyUSB events (`usb.cpp`, handled in
`pc_state.cpp`). Confirmed on the bench (2026-09-30, then under Arduino) and
on the assembled board:

| Event | Link state | Meaning |
|---|---|---|
| `TINYUSB_EVENT_ATTACHED` / `TINYUSB_EVENT_RESUMED` | Active | PC on |
| `TINYUSB_EVENT_SUSPENDED` | Suspended | PC asleep or off |
| `TINYUSB_EVENT_DETACHED` | -- | Logged only |

So on/off/asleep detection works over the module's existing USB link to the
motherboard's internal header, with no extra wiring (no Power LED or
PWR_OK tap).

It needs the USB-OTG peripheral (TinyUSB), which the firmware always uses;
the USB-Serial/JTAG peripheral does not report these events. `pcsim
on|off|auto` overrides the state for tests.

## The cold-boot blip and the 30 s guard

A cold boot blips the link on and off while the BIOS re-enumerates USB
during POST, before the OS is up. Taken at face value that looks like a
real suspend, and the displays would flicker off on every boot.

So every time the link goes Active (fresh boot or resume) a 30 s guard
starts:

- Link Active -> PC on, screens on immediately, guard starts.
- A Suspend inside the guard is logged and ignored (assumed POST noise).
- After 30 s, a Suspend is trusted immediately -> PC asleep/off,
  backlights off.

Only the active side is debounced: the false positive is specific to boot
noise, not to shutdown or sleep.

## Implementation

[`firmware/main/pc_state.cpp`](../firmware/main/pc_state.cpp).
The `status` command shows the state and `(guard)` while the guard runs;
the status view shows a countdown.

## Open items

- The guard also applies to resume from sleep, on the assumption that
  resume can blip the bus too. Only the cold-boot blip has been observed.
- A Power LED / PWR_OK tap would still be a useful second signal that does
  not depend on USB enumeration timing, but it is no longer required.


## Restart / update announcements

PcHwCtl (the PC hardware service, a separate repo) watches the System event log (1074 and the Kernel-Power and EventLog shutdown events) and the Windows Update registry keys, and writes what it finds to its state file. The PC helper (`host/pc_stats`) reads that file every second and passes the keys on to the board, renamed from `hw.*` to `pc.*`, before the USB link drops (it re-reads at once on suspend and shutdown). Full key list: [host/pc_stats/README.md](../host/pc_stats/README.md#restart--update-detection).

- `@t pc.going <kind>`: `restart`, `shutdown`, `update_restart`, `update_shutdown`, `sleep`, `hibernate`, or `none`.
- `@t pc.going_by <process>`: e.g. `MoUsoCoreWorker.exe` (`-` if none).
- `@t pc.going_why <reason>`: reason text (`-` if none).
- `@s pc.going_id=<n>`: id of the announcement (0 = none/cancelled). A change in id triggers latching.
- `pc.update_pending`: a Windows update is waiting for a reboot; `pc.update=1`: Windows is installing updates right now.
- `@t pc.last_down <kind>`: previous boot's shutdown cause (also `bluescreen`, `power_button`, `unexpected`, `unknown`).
- `@t pc.last_down_at <text>`: local timestamp of the last down event (`-` if unknown).

While PcHwCtl is down the helper sends `none` / 0 / `unknown`, so the board holds no stale announcement.

### Board States and Windows

When a valid `pc.going_id` arrives with a known kind, the carrier firmware latches the announcement, logs it, and dispatches a Discord notification:
- **Restart (`restart` / `update_restart`)**: The firmware expects the PC to reboot and re-enumerate USB. Once the PC drops to `PC_OFF`, a window opens: 10 min for a restart (`PCGOING_BACK_MS`), 2 h for an update restart (`PCGOING_BACK_UPDATE_MS`, which can reboot several times). The PC counts as back when it is on and fresh stats arrive with `pc.update` at 0; the board then posts `PC back after restart (N min N s)` (or `after updates`). If the window expires first, it logs `pc: did not come back` and clears.
- **Updates reported without an announcement**: `pc.update=1` on its own latches an update restart already under way ("PC installing updates").
- **Shutdown / Sleep / Hibernate / Update Shutdown**: The update work happens before the PC goes off; the latch clears 5 s after the PC enters `PC_OFF`, and normal sleep/off display handling takes over.
- **Timeout while ON**: If an announcement is latched but the PC stays `PC_ON` (and is not installing updates), it times out and clears after 2 min (`PCGOING_PREOFF_MS`), or 60 min for an update restart or shutdown (`PCGOING_PREOFF_UPDATE_MS`).
- **Cancellation**: If `pc.going_id` goes back to 0 while latched before the PC powers off, the firmware clears the latch and logs `pc: shutdown cancelled` (or posts `PC back after restart` when the PC's uptime shows it already rebooted).

### Screen Hold

During restart and update cycles (`restart`, `update_restart`, `update_shutdown`), `pcGoingHoldsScreens()` remains true while the PC is off. Backlights remain active (`backlightIdle` is suppressed). Panels on an auto view (`pc`, `media`, `ai`) show the restart or update themselves (a GIF with a "PC is Restarting" / "PC is Updating" box, the clock with the state); any other view is saved in RAM and the panel switches to `VIEW_STATUS`. Once the cycle concludes or the PC wakes, the saved views and regular backlight idle states are restored.

### Wake Guard

While restart or update announcements are active, `pcGoingWakeBlock()` prevents accidental wake pulses (e.g. via `wake` or the Discord Wake button), responding with reasons such as `PC is restarting (back in ~2 min)` or `PC is installing updates (12 min)`. The guard can be overridden from the console using `wake force`.

### Testing

The firmware provides a test command to simulate host announcements:
```
going <restart|shutdown|update_restart|update_shutdown|sleep|clear>
```
Injecting a state acts as if announced with initiating process `test` and reason `injected`. `going clear` resets the latch immediately.
