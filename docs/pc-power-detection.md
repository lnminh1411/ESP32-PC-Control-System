# PC power-state detection over USB

The ESP32-S3's native USB-OTG peripheral, in TinyUSB mode, reports the host
bus state through Arduino's `USB.onEvent()` callbacks. Confirmed on the
bench (2026-09-30) and on the assembled board:

| Event | Link state | Meaning |
|---|---|---|
| `ARDUINO_USB_STARTED_EVENT` / `ARDUINO_USB_RESUME_EVENT` | Active | PC on |
| `ARDUINO_USB_SUSPEND_EVENT` | Suspended | PC asleep or off |
| `ARDUINO_USB_STOPPED_EVENT` | -- | Never seen in testing; logged only |

So on/off/asleep detection works over the module's existing USB link to the
motherboard's internal header, with no extra wiring (no Power LED or
PWR_OK tap).

Needs `USBMode=default` (USB-OTG/TinyUSB) in the board config. The
USB-Serial/JTAG mode does not report these events.

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

[`firmware/carrier_main/pc_state.cpp`](../firmware/carrier_main/pc_state.cpp).
The `status` command shows the state and `(guard)` while the guard runs;
the status view shows a countdown.

## Open items

- The guard also applies to resume from sleep, on the assumption that
  resume can blip the bus too. Only the cold-boot blip has been observed.
- A Power LED / PWR_OK tap would still be a useful second signal that does
  not depend on USB enumeration timing, but it is no longer required.


## Restart / update announcements

A Windows service on the host (`host/pc_stats`) monitors System-log Event 1074 (User32) to distinguish shutdowns, restarts, updates, and sleep before the USB link drops. It streams announcements as `@s` and `@t` keys over USB HID:

- `@t pc.going <kind>`: `restart`, `shutdown`, `update_restart`, `update_shutdown`, `sleep`, or `none`.
- `@t pc.going_by <process>`: e.g. `MoUsoCoreWorker.exe` (`-` if none).
- `@t pc.going_why <reason>`: reason text, up to 60 characters (`-` if none).
- `@s pc.going_id=<n>`: EventRecordID of the 1074 event or fallback sequence ID (0 = none/cancelled). A change in ID triggers latching.
- `@s pc.update_pending=0|1`: Windows update pending reboot.
- `@t pc.last_down <kind>`: previous boot's shutdown cause (`restart`, `shutdown`, `update_restart`, `update_shutdown`, `unexpected`, `unknown`).
- `@t pc.last_down_at <text>`: local timestamp of the last down event (format `ddd HH:mm`, `-` if unknown).

### Board States and Windows

When a valid `pc.going_id` arrives with a known kind, the carrier firmware latches the announcement, logs it, and dispatches a Discord notification:
- **Restart (`restart` / `update_restart`)**: The firmware expects the PC to reboot and re-enumerate USB. Once the PC drops to `PC_OFF`, a timeout window opens: 180 s for standard restarts, or 45 min for update restarts. When the PC returns (`PC_ON`), it logs and notifies `PC back after restart (N s)`. If the window expires without the PC returning, it logs `pc: did not come back` and clears.
- **Update Shutdown (`update_shutdown`)**: Retains `UPDATING` state for up to 45 min while Windows completes offline servicing before powering off.
- **Shutdown / Sleep (`shutdown` / `sleep`)**: Clears 5 s after the PC enters `PC_OFF`, allowing normal sleep/off display handling to take over.
- **Timeout while ON**: If an announcement is latched but the PC remains `PC_ON` for more than 120 s without dropping USB, the announcement times out and clears.
- **Cancellation**: If `pc.going_id` reverts to 0 while latched before the PC powers off, the firmware clears the latch and logs `pc: shutdown cancelled`.

### Screen Hold

During restart and update cycles (`restart`, `update_restart`, `update_shutdown`), `pcGoingHoldsScreens()` remains true while the PC is off. Backlights remain active (`backlightIdle` is suppressed), the current view on each panel is saved in RAM, and all panels enter `VIEW_STATUS` showing `RESTARTING` or `UPDATING` with remaining time. Once the cycle concludes or the PC wakes, the saved views and regular backlight idle states are restored.

### Wake Guard

While restart or update announcements are active, `pcGoingWakeBlock()` prevents accidental wake pulses (e.g. via `wake` or the Discord Wake button), responding with reasons such as `PC is restarting (back in ~2 min)` or `PC is installing updates (12 min)`. The guard can be overridden from the console using `wake force`.

### Testing

The firmware provides a test command to simulate host announcements:
```
going <restart|shutdown|update_restart|update_shutdown|sleep|clear>
```
Injecting a state acts as if announced with initiating process `test` and reason `injected`. `going clear` resets the latch immediately.
