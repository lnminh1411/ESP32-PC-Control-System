# PC helper (ESP32-Helper.exe)

The Windows side: it feeds the board the PC's stats, media and AI quota, and
carries the board's commands to PcHwCtl and LocalAIHost. The full reference
is [host/pc_stats/README.md](../host/pc_stats/README.md); this page is the
overview.

`host/pc_stats` builds one program, `ESP32-Helper.exe`, a self-contained
single-file .NET 10 exe (needs the .NET 10 SDK to build, nothing to run;
hardware sensors come from PcHwCtl's sensor feed, `E:\PC Hardware
Controller`, the only LibreHardwareMonitor reader on the PC). It runs as two
processes:

- **The Windows service** (SYSTEM, session 0): reads PcHwCtl's sensor feed
  and state file and Windows performance counters, forwards the board's power,
  RGB, fan, monitor and night mode commands to PcHwCtl, polls the local AI
  status hub and Claude's usage, and streams everything to the board over a
  WinUSB bulk pipe (no driver install; vendor HID as the fallback; the COM
  port stays free either way). It also renders
  non-Latin text for the board and sends cover art.
- **The user-session half** (`--user`, started and restarted by the service
  in the signed-in user's session, as that user): media info from Windows'
  media session (Spotify, browsers), Antigravity quota, the Claude Code login
  refresh, and board-requested screenshots. Session 0 can't see the desktop
  or media sessions, hence the second process. The two talk over anonymous
  pipes.

## Details worth knowing

- **CPU load** uses `% Processor Time`. `% Processor Utility` (what Task
  Manager shows) is scaled by clock speed and goes past 100% when the CPU
  boosts.
- **GPU load** is averaged over ~2 s on the board. At idle clocks the
  RTX 3050 reports bursty utilisation: it renders the desktop and wallpaper
  at 210-315 MHz, so the raw value jumps between 0 and 50%.
- **Claude usage** comes from the same endpoint Claude Code uses. The
  helper refreshes the Claude Code login shortly before it expires and
  writes the new token back to `~/.claude/.credentials.json`, so the card
  keeps working when Claude Code is not running.
- **Antigravity quota:** the 5-hour Flash window comes from Google's API
  with agy's stored login (the OAuth client is read out of `agy.exe`), and
  from a running Antigravity client's local language server when there is
  one. The weekly Gemini window is only shown in agy's
  own `/usage` panel, so every 10 minutes the helper runs `agy` in a pseudo
  console, types `/usage` and reads the result. It then removes the lines
  this adds to agy's prompt history.
- **Restarts, shutdowns and Windows updates** are detected by PcHwCtl (the
  System event log and the update registry keys); the service passes its
  state on as `pc.going*`, `pc.last_down*`, `pc.update_pending` and
  `pc.update` (installing right now). The board keeps its Updating faces
  until `pc.update` goes back to 0 and the PC is back. While PcHwCtl is down
  the board gets `none` / 0 / `unknown` rather than stale values.
- Tokens are never logged.

## Build and install

```bat
host\pc_stats\build.cmd
powershell -ExecutionPolicy Bypass -File host\pc_stats\install.ps1
```

`build.cmd` needs the .NET 10 SDK. It turns the `HOST_*` settings into
`Settings.g.cs` first (no other key, so no secret reaches the exe), then
publishes `build\ESP32-Helper.exe`. `install.ps1` (elevated) copies it to
`C:\Program Files\ESP32 PC Controller\pc_stats\`, keeps an existing
`stats.ini` and `agy_quota.ini` unless given `-ForceConfig`, and sets up the
`ESP32PcStats` service to start automatically and restart on failure.
`uninstall.ps1` removes it again; the log in `C:\ProgramData\ESP32PcStats`
stays.

See [host/pc_stats/README.md](../host/pc_stats/README.md) for `stats.ini`, the
wire protocol, the night pass, install (`install.ps1`, elevated) and
command-line options.
