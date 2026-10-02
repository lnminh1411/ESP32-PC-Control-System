# PC Power-State Detection via USB PHY (2026-09-30)

## Finding

Confirmed on bench: the ESP32-S3's native USB-OTG peripheral, in TinyUSB
mode, reports host bus state through Arduino's `USB.onEvent()` callbacks.
Mapped:

- `ARDUINO_USB_STARTED_EVENT` / `ARDUINO_USB_RESUME_EVENT` -> USB Active -> PC ON
- `ARDUINO_USB_SUSPEND_EVENT` -> USB Suspend -> PC asleep or off
- `ARDUINO_USB_STOPPED_EVENT` -> never actually observed in testing; logged
  for diagnostics only, not used for any decision

This means PC on/off/asleep detection is achievable through the S3's
existing USB link (USB-C to the motherboard's internal header) with zero
new hardware. No Power LED / ATX PWR_OK tap is required just for this.

## Problem: cold-boot USB blip

Cold boot causes a brief USB on/off blip on the S3's link (BIOS POST
re-enumerating the USB controller before the OS is up). Taken at face
value, that blip is indistinguishable from a real suspend event -- a naive
suspend-to-idle transition would flicker the displays on every cold boot.

## Fix: 30s guard window

State machine, (re)armed every time the link goes ACTIVE (fresh boot or
resume from sleep):

- USB goes Active -> monitoring screens turn on immediately, 30s guard
  starts.
- Any Suspend event landing inside the 30s guard is logged but ignored --
  state stays ACTIVE (assumed POST noise, not a real suspend).
- Once the 30s guard elapses uninterrupted, Suspend events are trusted and
  acted on immediately, no further debounce -> screens switch to idle mode
  (weather/etc. placeholder on the bench rig; real target is all 3 LCDs).

No guard on the suspend-to-idle side -- only the active-side transition
needs debouncing, since the false-positive risk is specific to boot-time
bus noise, not to shutdown/sleep itself.

## Test firmware

[`firmware/s3_usb_state_test/s3_usb_state_test.ino`](../firmware/s3_usb_state_test/s3_usb_state_test.ino)
implements the above. Logs every transition with a timestamp to both
Serial and the bench display, so the log survives the period where USB
itself is suspended and unreadable.

Requires non-default board USB config -- see the file header for the
exact `USBMode`/`CDCOnBoot` build options. Incompatible with the plain
`s3_display_fps_test.ino` config: different USB peripheral in use
(native USB-Serial/JTAG vs. USB-OTG/TinyUSB), can't run both configs off
the same flashed image.

## Open items

- Guard is applied uniformly to both fresh boot (`STARTED`) and
  resume-from-sleep (`RESUME`), on the assumption both can produce similar
  bus noise around the transition. Only the cold-boot blip has actually
  been observed/confirmed on the bench -- resume-side noise is an
  assumption, not yet tested.
- Bench-proven on a single test display only, not yet wired into the real
  3-display product firmware.
- Doesn't retire the Power LED / PWR_OK tap idea outright if that's wanted
  for other reasons (redundancy, or a lower-level signal that doesn't
  depend on USB enumeration timing) -- just means it's no longer a hard
  requirement for on/off detection specifically.
