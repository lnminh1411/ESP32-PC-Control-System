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
update and home's warnings while you are away at quiet (-1). See
[docs/weather.md](weather.md) for the sources, call budgets and
thresholds.

## Control panel

The panel (#control, or `/actions` for one that only you see) opens with a
category dropdown; picking one swaps the controls below it:

| Tab | Shows | Controls |
|---|---|---|
| Door | Door remote state | Door 1-4 |
| PC Control | PC state, board Wi-Fi, night and low power mode, power plan, CPU, RAM, both GPUs, drive, fans, network | Wake PC, Lock; Sleep, Hibernate, Restart, Shut down, PC reset (these ask first); Screenshot; Night auto on/off; Start / End night now; Low power on/off |
| PC Case RGB LEDs | PcHwCtl's RGB rules and brightness | Mode, color and brightness dropdowns; target group (All, Motherboard, RAM) |
| PC Fan Control | Each fan's speed, rpm and temperature, the RX 6600 and RTX 3050 included (with Zero RPM) | Target (All fans, Case fans, one case fan, RX 6600, RTX 3050), speed (held 5/15/60 min) and curve dropdowns; a Zero RPM dropdown in place of the curve for a GPU; Auto releases the override |
| PC Monitor Control | Each monitor's power and brightness, numbered from the left | Target (one monitor or all), brightness in 10 % steps (DDC/CI); All screens on / off (each monitor's power method in PcHwCtl, plus Windows' display off) |
| Local Agent Control | Local model, phase, context, auto-unload, keep awake, services, VRAM | Load, unload, force unload and reload (both ask first), warm cache; context 128K / 256K, auto-unload, keep awake; restart the gateway (asks first), Web UI or proxy; guard check |
| ESP32 Board Control | Uptime, Wi-Fi, internal RAM, PSRAM, last restart reason, status LED | Status LED max brightness (10 % steps) and on/off (both saved); Restart board (asks first), Refresh weather, Reconnect Discord |
| ESP32 Display Control | Each panel's screen and backlight | A screen dropdown per panel (each with an Auto option; not saved, like `face`); a brightness dropdown for all panels (5 % steps + Off, saved) |

Every tab has a Refresh and, on an `/actions` panel, a Close. The Display tab
fills all five of Discord's action rows with dropdowns, so Refresh there is an
entry inside the category dropdown instead of a button.

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
image) and posts it to a Discord webhook for 5 minutes, then deletes the post;
the local file in `%temp%` is removed as soon as it is uploaded. The board only
asks (`!shot` over the USB link, with the webhook from settings.h); the capture, overlay
and upload run in the signed-in session, because the SYSTEM service cannot see
the desktop. Right after the capture, a red border and "Screenshot Captured" in
red show on every monitor for 5 s - these are drawn *after* the capture, so they
are never in the image. If nobody is logged in (locked / login screen) it just
logs and does nothing; two presses close together are coalesced (the second is
dropped while the first is in flight).

Set it up with one line in settings.h (keeps the config in one place):

1. In Discord: #logging -> Edit Channel -> Integrations -> Webhooks -> New
   Webhook -> Copy Webhook URL.
2. `#define DISCORD_SHOT_WEBHOOK "https://discord.com/api/webhooks/..."` in settings.h.

Empty (the default) disables the button: it answers "no screenshot webhook in
settings.h".
