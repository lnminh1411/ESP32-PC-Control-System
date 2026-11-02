# Security

Audit of 2026-11-02 (four passes: board network, board local, helper, secrets
and scripts). What was fixed is in the code and the other docs; this page keeps
the risks left open on purpose and what to check if an attack is suspected.

## Accepted risks

| Risk | Why it stays |
|---|---|
| `E:\` lets any signed-in account write the repo (settings.h, build scripts, `install.ps1`, PcHwCtl / LocalAIHost sources) | `E:\` is the coding partition. Checked when an attack is suspected (below) instead of locked down. |
| ROM download mode and no flash encryption / secure boot eFuses | Anyone with the board in hand can read settings.h's secrets off flash. Burning eFuses is one-way and ends USB flashing as used here. |
| The board console (UART, CDC, vendor, HID) has no login | It takes physical access or a local process; the worst it can do is power the PC off. A local process can also fake a presence answer. |

## Where secrets live

| Secret | In |
|---|---|
| Discord token, Wi-Fi, MQTT, Pushover, OWM / Tomorrow keys, `OTA_PASS`, `OWNTRACKS_SECRET`, `LINK_KEY` | `firmware/main/settings.h` (git-ignored), compiled into the board firmware |
| `DISCORD_SHOT_WEBHOOK`, `LINK_KEY` | Also compiled into `ESP32-Helper.exe`; install folder is SYSTEM + Administrators only |
| Core dumps | Hold the Discord token: erased after the boot summary unless `COREDUMP_KEEP` |

## If an attack is suspected

1. `E:\` ACLs: `icacls E:\` and `icacls "E:\ESP32 PC Controller"`; look for
   accounts other than yours, SYSTEM and Administrators with write access.
2. `git status` and `git log -p -5` in this repo, PcHwCtl and LocalAIHost: any
   change you didn't make, above all in `install.ps1`, `build.cmd`,
   `gen_settings.ps1`, `scripts/`, `idf_patches/`.
3. settings.h: modified time, and that no key points somewhere new (webhooks,
   `MQTT_URI`, `WOL_TARGET`).
4. Installed exes against fresh builds: `Get-FileHash` on
   `C:\Program Files\ESP32 PC Controller\pc_stats\ESP32-Helper.exe`, PcHwCtl's
   and LocalAIHost's exes; `sc.exe qc` for each service's binary path.
5. `icacls "C:\Program Files\ESP32 PC Controller\pc_stats"` and
   `icacls C:\ProgramData\ESP32PcStats`: SYSTEM and Administrators only.
6. Helper log: `board: line before the link key check`, `board: wrong link
   key answer`, `pipe is not served by the ... service`, unrecognized board lines.
7. Rotate: Discord token and webhook, `OTA_PASS`, `LINK_KEY`,
   `OWNTRACKS_SECRET` (phone too), MQTT and Pushover credentials; rebuild and
   flash the board, rebuild and install the helper.
