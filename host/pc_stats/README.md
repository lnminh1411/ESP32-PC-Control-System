# ESP32 PC Stats Service

Windows service that reads hardware sensors via LibreHardwareMonitor (`lhwm-wrapper.dll`, OpenRGB build) and streams configured stats to the ESP32-S3 carrier board over a vendor-defined USB HID interface (with CDC serial fallback).

## Transport

The service streams stats over a custom USB HID vendor interface (UsagePage `0xFF00`, Usage `0x01`, report length 64 bytes) on the ESP32-S3 (Espressif VID `303A`).
- **Why HID:** Windows automatically binds its built-in in-box HID driver without prompting for a driver install. The board's native USB CDC COM port remains completely free for firmware flashing and serial monitor consoles.
- **Serial Fallback:** If no matching HID vendor interface is detected, or if `--transport serial` is requested, the service falls back to streaming over the USB CDC COM port (`@s`, `@t`, `@e` lines).

## Build

Run `build.cmd` from Command Prompt:
```cmd
host\pc_stats\build.cmd
```
Builds `build\esp32-pcstats.exe` using Microsoft .NET Framework 4 `csc.exe` (ships with Windows). On the first run, it automatically downloads `lhwm-wrapper.dll` into `vendor\`.

## Installation & Removal

Run PowerShell as Administrator:
- **Install**: `.\install.ps1` (or `.\install.ps1 -ForceConfig` to overwrite existing `stats.ini`).
  Installs files to `C:\Program Files\ESP32 PC Controller\pc_stats\` and creates/starts service `ESP32PcStats` with automatic restart on failure.
- **Uninstall**: `.\uninstall.ps1`
  Stops and deletes the service and removes the installation directory (preserving `C:\ProgramData\ESP32PcStats\log.txt`). The log rotates at 4 MB into `log.old.txt`, so the two stay under 8 MB together.

## Command-Line Usage

```cmd
esp32-pcstats.exe                      # Run as Windows Service
esp32-pcstats.exe --console            # Run interactively, printing stats each tick
esp32-pcstats.exe --list               # Print all available hardware sensors and exit
esp32-pcstats.exe --bench              # Benchmark sensor update latencies
esp32-pcstats.exe --transport auto     # Transport mode: auto (default), hid, or serial
esp32-pcstats.exe --port COM4          # Manually specify COM port instead of VID_303A auto-detect
esp32-pcstats.exe --claude <dir>        # Test Claude live fetch & local token totals and exit
```

## stats.ini Format

`stats.ini` lives alongside `esp32-pcstats.exe` and is monitored for changes every tick (hot-reloaded on write):
```ini
# Comments with '#', blank lines allowed. Keys: [a-z0-9_.] max 23 chars.
key = <hardware> | <sensor type> | <sensor name> [| first|avg|sum|max|min]
key = @uptime                                (PC uptime in seconds via GetTickCount64)
key = @drive <letter>: used|free|total|load  (GB, load = used %, via DriveInfo)
key = @cpu_mhz                               (Task Manager's CPU speed, free)
key = @counter <category> | <counter> | <instance> [| scale]   (Windows perf counter)
@interval <ms>                               (sample interval, 200-10000, default 500)
@every <hardware> <ms>                       (update slow hardware at most every ms)
@source <name> <url> <ms>                    (poll HTTP key=value stream on background thread)
@claude <claude_dir> <live_ms>              (poll Claude live usage & scan local token spend)
```
- `<hardware>`: Comma-separated list of hardware type (e.g. `Cpu`, `GpuAmd`, `GpuNvidia`, `GpuIntel`, `Memory`, `Network`, `SuperIO`, `Motherboard`, `Storage`) or hardware name. Supports `*` wildcards. Sub-hardware (e.g. SuperIO under Motherboard) is searched recursively.
- `<sensor type>`: `SensorType` enum name (`Temperature`, `Load`, `Clock`, `Fan`, `Data`, `SmallData`, `Throughput`, ...).
- `<sensor name>`: Sensor name from LibreHardwareMonitor. Supports `*` wildcards.
- Reduction (`first|avg|sum|max|min`): Defaults to `first`. Empty/null sensor readings are skipped.
- `@source <name> <url> <ms>`: Spawns a background thread to poll an HTTP endpoint returning `key=value` lines (e.g. `@source ai http://127.0.0.1:8080/_gate/ai/flat 1000`). Numeric values join the `@s` stream; text values stream as `@t` lines. Generates `<name>.ok` (1 if updated within 3 x interval, 0 on timeout/error, dropping other source keys when down).
- Run `esp32-pcstats.exe --list` to inspect the exact hardware type/name and sensor names available on your system.

## Claude Usage

The service can stream Claude Code live utilization and local spend totals to the ESP32. Configure it in `stats.ini`:
```ini
@claude <claude_dir> <live_ms>
# e.g.:
@claude C:\Users\lnminh1411\.claude 120000
```

### What It Reads
- **Live Usage**: Reads the OAuth access token from `<claude_dir>\.credentials.json` (`claudeAiOauth.accessToken`) and polls Anthropic's OAuth usage API (`https://api.anthropic.com/api/oauth/usage`). It only uses the access token that Claude Code itself keeps fresh (never refreshes it, never uses the refresh token, and never writes to the credentials file). If the token is expired (`claudeAiOauth.expiresAt`), polling pauses with error `token_expired`. The per-user helper `esp32-agyquota.exe` prevents that: when less than 10 minutes are left it runs `claude auth status`, which makes the Claude Code CLI refresh its own token (no model call, no usage). It reads only the expiry times, and logs to `agy_quota.log` when the refresh token itself nears expiry (then run `claude auth login`).
- **Local Spend & Token Totals**: Incrementally scans session transcripts in `<claude_dir>\projects\**\*.jsonl` modified in the last 31 days using an in-memory per-file byte cache. Tokens are calculated as `input_tokens + output_tokens + cache_creation_input_tokens + cache_read_input_tokens`, de-duplicated by `message.id`. Cost is estimated at standard list rates (Opus, Sonnet, Haiku).

### Keys Streamed
- **Numeric stats (`@s`)**:
  - `claude.ok`: `1` if live data is fresh (fetched within 3 x `live_ms`), else `0`
  - `claude.stale`: `1` if showing last-known live values after an error
  - `claude.s_pct`: Session (5h) utilization %
  - `claude.s_reset_s`: Seconds until session (5h) window resets
  - `claude.s_pace`: Projected % at session reset (omitted if window too new)
  - `claude.w_pct`: Weekly (7d) utilization %
  - `claude.w_reset_s`: Seconds until weekly reset
  - `claude.w_pace`: Projected weekly % at reset
  - `claude.w_<model>_pct`: Per-model weekly limit % (e.g. `claude.w_sonnet_pct`)
  - `claude.x_pct`: Extra usage % (when enabled)
  - `claude.tok_5h`: Tokens used in the last 5 hours (local logs)
  - `claude.tok_today`: Tokens used since local midnight
  - `claude.tok_7d`: Tokens used in the last 7 days
  - `claude.tok_30d`: Tokens used in the last 30 days
  - `claude.usd_today`: Estimated cost ($) since local midnight
  - `claude.usd_30d`: Estimated cost ($) in the last 30 days
- **Text stats (`@t`)**:
  - `claude.plan`: Subscription / tier plan name (e.g. `Pro`, `Max 5x`)
  - `claude.s_reset_at`: Local time of session reset (`HH:mm`)
  - `claude.w_reset_at`: Local weekly reset (`ddd dd MMM HH:mm`, InvariantCulture)
  - `claude.s_pace_v`: Session pace verdict (`ok`, `tight`, `over`)
  - `claude.w_pace_v`: Weekly pace verdict (`ok`, `tight`, `over`)
  - `claude.err`: Error status (`""` when fine, else `token_expired`, `not_logged_in`, `rate_limited`, `request_failed`, `no_credentials`)

Usage logic ported from aiusage (MIT, Ahsan Habib Akik).

## Antigravity Quota

The companion helper `esp32-agyquota.exe` runs in the user's interactive session and queries the local language server exposed by running Antigravity clients (the IDE, or an `agy` CLI session for another account) over Connect-JSON API on `127.0.0.1`. It writes quota metrics to `C:\ProgramData\ESP32PcStats\agy_quota.txt`, which `esp32-pcstats.exe` polls and forwards via directive:
```ini
@source agy file:///C:/ProgramData/ESP32PcStats/agy_quota.txt 5000
```

### How It Works
- **Running Client Required:** The helper cannot retrieve credentials directly from LocalSystem service context; it discovers running `language_server.exe` (IDE) and `agy.exe` (CLI) processes via WMI, finds their listening localhost TCP ports via `GetExtendedTcpTable`, and queries `GetUserStatus` with the CSRF token extracted from the process command line (tokens are never logged or written). A running Antigravity client logged into an account is required to refresh that account's quota. When a client closes, its last-known quota values are preserved while `age_s` increments.
- **Accounts & Slots:** Mapped in `agy_quota.ini` (`accounts = ...`) to slots `agy1`, `agy2`, etc. Defaults to accounts in `%USERPROFILE%\.gemini\antigravity-cli\accounts\`.
- **Model Families & Regexes:** Models are classified by label (case-insensitive) using regexes configured in `agy_quota.ini`:
  - `exclude = claude|gpt|oss`
  - `flash = flash`
  - `pro = \bpro\b|argon|ultra`
  For each family, the lowest remaining quota fraction and earliest reset time among active models are tracked. A family without quota models omits numeric keys and sends only its display name (e.g. disabled Argon).
- **Scheduled Task:** `install.ps1` registers a scheduled task `ESP32 AGY Quota` that runs `esp32-agyquota.exe` at logon of the installing user with Limited privileges, no execution time limit, and automatic restart on failure.

### Keys Streamed
- **Global:**
  - `agy.live`: Count of active language server clients answering the poll
  - `agy.updated`: Unix timestamp (seconds) of the poll
- **Per-Slot (`agy1`, `agy2`, ...):**
  - `agyN.email`: Account email address (`@t`)
  - `agyN.plan`: Plan name (e.g. `Pro`) (`@t`)
  - `agyN.age_s`: Elapsed seconds since account data was fetched
  - `agyN.flash_pct`: Remaining Flash quota percentage (`0.0` - `100.0`, 1 decimal)
  - `agyN.flash_reset_s`: Seconds until Flash quota reset (`>= 0`)
  - `agyN.flash_reset_at`: Local time of Flash reset (`ddd HH:mm`, InvariantCulture) (`@t`)
  - `agyN.flash_name`: Flash family display name (`@t`)
  - `agyN.pro_pct`, `agyN.pro_reset_s`, `agyN.pro_reset_at`, `agyN.pro_name`: Same metrics for Pro family

## Wire Protocol

ASCII lines streamed every `@interval` ms (default 500):
```
@s key=value key=value ...
@t key text
@e <seq>
```
- `@s`: Space-separated `key=value` numeric tokens, split into lines of at most 300 characters. Value formatted as float `G7` (InvariantCulture), or empty if no reading (e.g. `gpu1_fan=`).
- `@t <key> <text>`: Text stats (rest of line up to 80 chars, newlines/tabs converted to spaces, e.g. for `ai.tool` or `ai.phase_name`).
- `@e <seq>`: End of sample packet with an incrementing sequence number.

When transported over the vendor HID interface:
Each packet is chunked into 63-byte pieces; each report starts with Report ID `0x06` followed by up to 63 payload bytes, zero-padded to 64 bytes total. The firmware discards zero bytes and reassembles lines identically.

## USB COM Port Notice

When connected over the vendor HID interface (default in `auto` mode), the ESP32-S3's USB CDC COM port is completely unused by the service. Firmware can be flashed and serial consoles opened without stopping the service.

If the serial fallback is in use (or forced with `--transport serial` or `--port`), the service holds an open handle on the CDC COM port. In that case, stop the service before flashing:
```cmd
sc.exe stop ESP32PcStats
```
After flashing:
```cmd
sc.exe start ESP32PcStats
```
