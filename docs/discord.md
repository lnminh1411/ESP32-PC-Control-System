# Discord and Pushover

The board runs its own Discord bot: an outbound gateway websocket from the
board, so nothing on the home network is opened. The keys it needs are in
[settings.md](settings.md#secrets).

## Channels

The Discord bot uses up to four channels, each set by its channel id in
`settings.h` and off when empty:

| Key | Channel | Posts |
|---|---|---|
| `DISCORD_CH_PC` | #pc-announcement | Board online, PC on/off, restart and update, power keys, door, reset |
| `DISCORD_CH_WEATHER` | #weather-announcement | Rain and typhoon warnings, "cleared" posts, OpenWeather alerts, a weather update every 2 h from 04:00 to 20:00 (the 04:00 one adds the 3-day forecast) |
| `DISCORD_CH_LOG` | #logging | The board's log, timestamped, in batches every 10 s (every 2 s while it is behind) |
| `DISCORD_CH_CONTROL` | #control | One control panel message with the `/actions` controls, kept up to date |
| `DISCORD_SHOT_WEBHOOK` | webhook URL | Where the Screenshot button posts (auto-deleted after 5 min); empty disables it |

PC and weather posts fall back to `DISCORD_CHANNEL_ID` when their own key is
empty. The bot also needs `DISCORD_TOKEN` (empty turns it off),
`DISCORD_APP_ID` and `DISCORD_GUILD_ID` (where `/actions` is registered). Only
`DISCORD_OWNER_ID` can use the buttons; anyone else gets "Not allowed.".

## Pushover

With `PUSHOVER_TOKEN` and `PUSHOVER_USER` set, every #pc-announcement and
#weather-announcement post is copied to Pushover (queued, one a second).
Weather warnings go out at high priority (`WEATHER_ALERT_PRIO`, 1: rings
through quiet hours), "cleared" posts and PC posts at normal, the 2-hourly
update and home's warnings while you are away at quiet (-1). Someone at the
desk while you are away goes out as an emergency (2), which repeats every 60 s
for an hour or until acknowledged (`PUSHOVER_EMERGENCY_RETRY_S`, `_EXPIRE_S`),
and leaves the board without waiting for the Discord gateway. See
[docs/weather.md](weather.md) for the sources, call budgets and
thresholds.

## Control panel

The panel (#control, or `/actions` for one that only you see) opens with a
category dropdown; picking one swaps the controls below it:

| Tab | Shows | Controls |
|---|---|---|
| Door | Door remote state | Door 1-4 |
| PC Control | PC state, board Wi-Fi, night and low power mode, power plan, CPU, RAM, both GPUs, drive, fans, network, presence at the desk | Wake PC, Lock; Sleep, Hibernate, Restart, Shut down, PC reset (these ask first); Screenshot; Presence, Auto night and Low power on/off; Night now / End night |
| PC Case RGB LEDs | PcHwCtl's RGB rules and brightness | Mode, color and brightness dropdowns; target group (All, Motherboard, RAM) |
| PC Fan Control | Each fan's speed, rpm and temperature, the RX 6600 and RTX 3050 included (with Zero RPM) | Target (All fans, Case fans, one case fan, RX 6600, RTX 3050), speed (held 5/15/60 min) and curve dropdowns; a Zero RPM dropdown in place of the curve for a GPU; Auto releases the override |
| PC Monitor Control | Each monitor's power and brightness, numbered from the left | Target (one monitor or all), brightness in 10 % steps (DDC/CI); a power button per monitor ("Mon 2: on", a press switches it through its power method in PcHwCtl); All on / All off (each monitor's power method, plus Windows' display off). With more than 4 monitors the per-monitor buttons give way to Target on / off |
| Local Agent Control | Local model, phase, context, auto-unload, keep awake, services, VRAM | Load, unload, force unload and reload (both ask first), warm cache, guard check; context (shows the next load's size, a press switches 128K / 256K), auto-unload, keep awake; restart (↻) the gateway (asks first), Web UI or proxy |
| ESP32 Board Control | Uptime, Wi-Fi, internal RAM, PSRAM, last restart reason, status LED | Status LED max brightness (10 % steps) and on/off (both saved); Reboot board (asks first), Weather ↻ (refresh), Discord ↻ (reconnect) |
| ESP32 Display Control | Each panel's screen and backlight | A screen dropdown per panel (each with an Auto option; saved like `face`, so a pinned screen stays across restarts); a brightness dropdown for all panels (5 % steps + Off, saved) |

Refresh and, on an `/actions` panel, Close are the last entries of the
category dropdown. Buttons come three to a row (Door: two), each label padded
with U+2800 blanks to the widest one on the panel, so every button has the same
width: a row fits a phone, and a row that still wraps keeps its columns. A
toggle that is on shows a ✓ and is blurple.

A button that asks first swaps the controls for "Yes, ..." and Cancel. On the
#control panel that prompt is left alone for 60 s (`CONTROL_PANEL_CONFIRM_MS`):
refresh edits and delayed redraws skip it, then the panel goes back to normal
without acting. The #control panel is edited every 5 s while it changes, for
3 min after a click (`CONTROL_PANEL_REFRESH_MS`, `CONTROL_PANEL_LIVE_MS`), else
every 10 min and on PC power changes; an edit Discord refuses posts a new panel
and deletes the old one. An `/actions` panel deletes itself after 2 min
without use.

## Screenshot

**Screenshot** (PC Control) captures the whole desktop (every monitor in one
image) and posts it to a Discord webhook for 5 minutes, then deletes the post.
The image stays in memory only. The board only asks (`!shot <nonce>` over the
USB link; the webhook never crosses USB), the helper adds the webhook it was
built with (`DISCORD_SHOT_WEBHOOK`, compiled in by `gen_settings.ps1`) and
passes it to PcHwCtl (`screenshot <nonce> <webhook>`, its `docs/ipc.md`) once
the pipe's server is the PcHwCtl service process. PcHwCtl captures, overlays,
uploads and deletes, and only posts to webhook ids listed in its
`pchwctl.json` (`screenshot.webhookIds`; elevated edit). Right after the
capture, a red border and "Screenshot Captured" in red show on every monitor for
5 s - these are drawn *after* the capture, so they are never in the image. If
the desktop is locked, the monitors are off or nobody is signed in it logs the
reason (`locked`, `display`, `agent`) in the helper log and does nothing; a
press while the last upload runs, or within 10 s of it, is declined as `busy`,
a webhook not on PcHwCtl's list as `webhook`.
PcHwCtl's config sets the times (`screenshot.deleteMin`, `borderS`).

Set it up with one line in settings.h (keeps the config in one place):

1. In Discord: #logging -> Edit Channel -> Integrations -> Webhooks -> New
   Webhook -> Copy Webhook URL.
2. `#define DISCORD_SHOT_WEBHOOK "https://discord.com/api/webhooks/..."` in settings.h.
3. Rebuild and install the helper (`build.cmd`, `install.ps1`) and the board.
4. Add the webhook's numeric id (the number after `/webhooks/`) to
   `screenshot.webhookIds` in `C:\ProgramData\PcHwCtl\pchwctl.json`.

Empty (the default) disables the button: it answers "no screenshot webhook in
settings.h".
