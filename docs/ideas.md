# Ideas, not specified yet

Raw capture. Don't build from this until it's turned into a spec.

## AI quota dashboard on one screen (2026-09-30)

Inspiration: OmarchyOS has a quota monitor on its status bar. A dedicated
physical screen could do the same.

- One panel shows Claude Code / Antigravity usage-quota status for 2
  accounts.
- Source: probably the `claude` and `agy` CLIs, if they expose quota/usage
  at all. Not checked yet.
- The ESP32 can't run those CLIs, so a companion program on the PC would
  poll them and send the numbers over the existing USB serial link (the
  same command channel the firmware already listens on).
- It needs a watchdog: the data isn't pushed, something has to poll it.

Open questions: do the CLIs expose quota data? Does the companion run as a
background service or on demand?
