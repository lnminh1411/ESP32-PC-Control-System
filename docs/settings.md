# Settings

Everything that describes this setup lives in one file,
`firmware/main/settings.h` (gitignored; copy `settings_example.h` to start).
The secrets come first (Wi-Fi, Discord, OTA, MQTT, Pushover, Wake-on-LAN,
OpenWeather, home place), then the rest of its 195 keys grouped by topic:
board pins and panels, Wi-Fi TX power (`NET_WIFI_TX_DBM`, 8 dBm), status LED,
night mode hours and levels, PC warning thresholds, GPU and drive labels,
quota card colours, PcHwCtl fan and RGB names, control panel timings, weather
and rain thresholds, poll periods and call budgets, OwnTracks, and the PC
helper's 32 `HOST_*` keys (Process Lasso path and night E-cores, heavy-exe
limits and ignore list, named pipes, adapters to skip, fonts, screenshot
timings, monitor DDC power). Every key has a default equal to the old
hardcoded value in `settings_or_example.h`, so a key missing from
`settings.h` behaves as before. Without a `settings.h` (CI) the build uses
`settings_example.h`: placeholders, nothing joins a network.

- After editing `settings.h`, touch `firmware/main/settings_or_example.h`
  before building (ccache's direct mode once handed back a stale image; a
  real image is ~1.6 MB, a placeholder one ~1.1 MB). CMake re-runs on its
  own when `settings.h` appears or goes away.
- `host/pc_stats/build.cmd` reads only the `HOST_*` keys
  (`gen_settings.ps1` -> `Settings.g.cs`), so no secret ends up in the exe.
  Changing one means rebuilding and reinstalling the helper.
- Which board key takes which PcHwCtl feed key (`cpu_temp`, `gpu1_*`, the
  fans, ...) stays in `host/pc_stats/stats.ini`, read at run time next to the
  exe; which LibreHardwareMonitor sensor feeds each key is PcHwCtl's config.
- `scripts/check_pins.py` checks the `PIN_*` settings against `pcb/main.ato`.

## Secrets

Only key names here; the values live in your `settings.h`. Empty means the
feature is off.

| Key | For |
|---|---|
| `WIFI_SSID`, `WIFI_PASS` | Wi-Fi (2.4 GHz) |
| `DISCORD_TOKEN`, `DISCORD_APP_ID`, `DISCORD_GUILD_ID`, `DISCORD_OWNER_ID` | The Discord bot ([discord.md](discord.md)); setup steps are in `firmware/main/discord.h` |
| `DISCORD_CHANNEL_ID`, `DISCORD_CH_PC`, `DISCORD_CH_WEATHER`, `DISCORD_CH_LOG`, `DISCORD_CH_CONTROL` | Channel ids |
| `DISCORD_SHOT_WEBHOOK` | The Screenshot button's webhook |
| `OTA_PASS` | `POST /ota` and `PUT /layout` |
| `MQTT_URI`, `MQTT_USER`, `MQTT_PASS`, `OWNTRACKS_TOPIC` | OwnTracks over MQTT ([weather.md](weather.md)) |
| `PUSHOVER_TOKEN`, `PUSHOVER_USER` | Pushover |
| `WOL_MAC`, `WOL_TARGET` | Wake-on-LAN before the helper has reported the PC's MAC; an extra address to send to |
| `WEATHER_HOME` | Home place, `lat,lon[,name]`, applied at boot |
| `OWM_API_KEY` | OpenWeather One Call 4.0 |

## Every other key

[settings-reference.md](settings-reference.md) lists the other keys with their
defaults and notes, one table per topic. It is generated from
`settings_or_example.h` by `python scripts/settings_doc.py`; run it again after
adding a setting.

`firmware/sdkconfig.defaults` (the ESP-IDF options) is summed up in
[firmware.md](firmware.md#esp-idf-configuration).
