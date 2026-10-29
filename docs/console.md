# Console commands

One per line on the USB CDC console or the CH343 UART (`help` lists them):

| Command | Does |
|---|---|
| `status`, `perf`, `pcstats [text]` | Board state; per-core CPU and bus load (run twice); stats received from the PC |
| `wifi scan` | The networks the board hears (channel, signal, security) and its allowed channels, then rejoins. A failed join logs its reason |
| `view <1\|2\|3> <pc\|media\|ai\|gif\|dash\|status\|stats\|metrics>` | What a panel shows (saved) |
| `face <1\|2\|3> <auto\|pc\|gif\|restart\|update\|media\|weather\|ai\|localai\|clock>` | Force one face, for testing and screenshots (not saved) |
| `shot <1\|2\|3>` | Screenshot of a panel, streamed as base64 on the CDC port |
| `pcsim <on\|off\|auto>` | Pretend the PC is on or off (LED, faces, backlights), for testing |
| `door <1-4> [ms]`, `reset`, `reboot` | Press a door button; pulse the motherboard reset (**resets the PC**); restart the board itself (the PC is not touched) |
| `screenshot` | Post the PC's desktop to #logging for 5 min, then auto-delete (needs the service + a #logging webhook) |
| `mon <bright <n\|all> <0-100> \| power all on\|off>` | The PC's monitors, numbered from the left: brightness over DDC/CI, power through each model's method plus Windows' display off for `all` (done by PcHwCtl, which the PC service forwards to) |
| `hw <ping \| rgb ... \| fan ...>`, `ai <status \| model ... \| restart <name> \| ...>` | Raw PcHwCtl (RGB, fans) and LocalAIHost commands, relayed by the PC service |
| `night [on\|off\|start\|end\|low on\|low off\|status]` | Night mode: on/off = the automatic engage (saved); start/end = now, until input on the PC; low = low power mode |
| `wake [force \| mac <AA:BB:..>]`, `power <sleep\|hibernate\|restart\|shutdown\|lock>` | Wake-on-LAN and USB remote wake; power actions, carried out by PcHwCtl (`lock` locks the signed-in session) |
| `going <restart\|shutdown\|update_restart\|update_shutdown\|sleep\|clear>` | Tell the board what the PC is about to do |
| `bl <1\|2\|3\|all> <0-100\|on\|off>`, `display <on\|off>`, `led <auto\|on\|off\|sweep\|0-100>` | Backlights (saved); all panels dark until `display on` (not saved); status LED |
| `fps [budget <20-95> \| cap <1\|2\|3> <1-60\|off>]` | SPI budget and per-panel frame caps |
| `pcwarn [set <metric> <high\|crit> <v\|off\|default> \| avg <ms> \| reset \| demo [one] \| off]` | PC usage warning limits and averaging window (saved in NVS) |
| `layout <show\|reload\|default\|begin\|data <b64>\|end>` | Dashboard layouts |
| `weather <show\|refresh\|demo [rain\|typhoon]\|clear\|storms\|home <lat>,<lon> [name]\|test-alert\|owm-check\|end-reset>` | Weather: state, refetch, 5-min demo, home place, a test alert, an OpenWeather merge test, reset the rain-end accuracy stats |
| `bench [all\|usage\|cpu\|mem\|flash\|spi\|usb <KB>\|psram [MB]]`, `socks` | Debug: memory map and task stacks, speed tests, PSRAM stress; open sockets |
