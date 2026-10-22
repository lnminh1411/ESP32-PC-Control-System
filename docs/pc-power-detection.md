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
