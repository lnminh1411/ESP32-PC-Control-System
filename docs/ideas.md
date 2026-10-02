# Ideas — Not Spec'd Yet

Raw capture, not designed. Don't build from this until it's turned into an
actual spec.

## 3rd screen: AI quota dashboard (2026-09-30)

Inspiration: OmarchyOS has a dedicated quota monitor on its status bar. If a
software status bar can do it, a dedicated physical screen can too.

Currently planned: 2 screens (per earlier 3-display circuit drop, still not
final). Idea for a 3rd:

- Dedicated to Claude Code / Antigravity usage-quota status, 2 accounts.
- Source: probably `claude` CLI and `agy` (Antigravity CLI) — whatever
  quota/usage info those expose, if any. Not confirmed either CLI actually
  surfaces this cleanly; needs checking.
- Needs a watchdog (this data isn't push-based, something has to poll it).
- Likely needs a dedicated companion program running on the PC side, since
  the ESP32 can't run these CLIs itself — PC program polls quota, hands it to
  the ESP32 over some link. Candidate transports, undecided: DLL (in-process,
  tightest integration, PC-only), serial (simplest, already have UART
  infra), USB-OTG (see [recovery-and-physical-access.md](recovery-and-physical-access.md)
  for why OTG mode trades away the native USB-Serial/JTAG debug interface —
  same tradeoff would apply here if OTG is used for this instead of a
  virtual NIC).

Open questions, not answered: does `claude`/`agy` CLI actually expose
quota/usage data at all? Where does the companion program run (background
service? on-demand)? Does this become a 4th "MCU role" or bolt onto
DevKit/S3's existing web UI split?
