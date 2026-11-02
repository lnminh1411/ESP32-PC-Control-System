# Settings

Everything that describes this setup lives in one file,
`firmware/main/settings.h` (gitignored; copy `settings_example.h` to start).
The secrets come first (Wi-Fi, Discord, HTTP password, MQTT, Pushover, Wake-on-LAN,
OpenWeather, Tomorrow.io, home place), then the rest of its 229 keys grouped by topic:
board pins and panels, Wi-Fi TX power (`NET_WIFI_TX_DBM`, 8 dBm), status LED,
night mode hours and levels, PC warning thresholds, GPU and drive labels,
quota card colours, PcHwCtl fan and RGB names, control panel timings, weather
and rain thresholds, poll periods and call budgets, OwnTracks, and the PC
helper's 30 `HOST_*` keys (Process Lasso path and night E-cores, heavy-exe
limits and ignore list, named pipes and their timeouts, adapters to skip,
fonts). Every key has a default equal to the old hardcoded value in
`settings_or_example.h` (except the eight core secrets, which come only from
`settings.h` or the example), so a key missing from `settings.h` behaves as
before. Without a `settings.h` (CI) the build uses
`settings_example.h`: placeholders, nothing joins a network.

- After editing `settings.h`, touch `firmware/main/settings_or_example.h`
  before building (ccache's direct mode once handed back a stale image; a
  real image is ~1.6 MB, a placeholder one ~1.1 MB). CMake re-runs on its
  own when `settings.h` appears or goes away.
- `host/pc_stats/build.cmd` reads only the `HOST_*` keys plus
  `LINK_KEY` and `DISCORD_SHOT_WEBHOOK` (`gen_settings.ps1` ->
  `Settings.g.cs`); no other secret ends up in the exe. Changing one means
  rebuilding and reinstalling the helper (`LINK_KEY`: the board too).
- `HOST_HWCTL_PIPE` is the pipe for every PcHwCtl call (sensor feed, power,
  screenshots, presence, RGB/fans, monitors, night mode), not only `!hw`.
  `HOST_LASSO_INI` is only read (Process Lasso's ProBalance exclusions);
  PcHwCtl writes the Lasso rules, and a leftover `prolasso.ini.esp32-night`
  from older helper code blocks night mode. `HOST_LHM_*` and `HOST_MON_*`
  are still generated but no helper code reads them: sensors and monitor
  DDC/CI moved to PcHwCtl, whose own config holds them now. The notes in
  settings-reference.md for these keys come from `settings_or_example.h`
  and still describe the old behaviour.
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
| `DISCORD_SHOT_WEBHOOK` | The Screenshot button's webhook. Compiled into both, but the board only checks it is set (empty = button off); the helper passes it to PcHwCtl, so it never crosses USB |
| `LINK_KEY` | Shared key the board proves to the helper before it is trusted ([helper.md](helper.md#build-and-install)); 64 hex chars, `python -c "import secrets;print(secrets.token_hex(32))"`; rebuild both |
| `OTA_PASS` | `PUT /layout` and `GET /bench` (header `X-OTA-Pass`; 5 wrong tries lock the server 30 s) |
| `MQTT_URI`, `MQTT_USER`, `MQTT_PASS`, `OWNTRACKS_TOPIC` | OwnTracks over MQTT ([weather.md](weather.md)) |
| `OWNTRACKS_SECRET` | OwnTracks payload encryption key, same as the app's ([weather.md](weather.md)) |
| `PUSHOVER_TOKEN`, `PUSHOVER_USER` | Pushover |
| `WOL_MAC`, `WOL_TARGET` | Wake-on-LAN before the helper has reported the PC's MAC; an extra address to send to |
| `WEATHER_HOME` | Home place, `lat,lon[,name]`, applied at boot |
| `OWM_API_KEY` | OpenWeather One Call 4.0 |
| `TOMORROW_API_KEY` | Tomorrow.io minutely rain |
| `COREDUMP_KEEP` | Not a secret, but in that section: `true` keeps the flash core dump after its summary is logged (for `idf_flash.py --idf coredump-info`); default `false` erases it, since a dump can hold secrets |

## Every other key

[settings-reference.md](settings-reference.md) lists the other 202 keys with
their defaults and notes, one table per topic. It is generated from
`settings_or_example.h` by `python scripts/settings_doc.py`; run it again after
adding a setting.

`firmware/sdkconfig.defaults` (the ESP-IDF options) is summed up in
[firmware.md](firmware.md#esp-idf-configuration).
