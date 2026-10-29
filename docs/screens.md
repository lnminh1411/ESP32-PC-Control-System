# Screens

![All screen faces, captured from the board](images/screens.png)

*Captured from the board itself with `shot` (see [build-and-flash.md](build-and-flash.md#screenshots)).
The stats in this image are made up; the layout is real.*

On its auto view (`view 1 pc`, `view 2 media`, `view 3 ai`; a fresh board
starts all three on `metrics` until a view is saved) each panel shows a set
of faces and picks one from the PC's state. Night mode shows the PC-off
faces:

| Panel | PC on | PC off / asleep | Restarting / updating |
|---|---|---|---|
| 1 | PC stats: CPU, RAM, both GPUs, fans, C: | GIF, scaled to fill the screen height | GIF with a "PC is Restarting" / "PC is Updating" box on top |
| 2 | Media: cover art, title, artist, player, progress; uptime and network | Weather: now, AQI, 2-hour rain nowcast, 3-day forecast | Weather |
| 3 | AI usage: Claude and Antigravity quota left, local AI; full-screen local AI face while the model is working | Clock | Clock with the restart/update state |

Notes on what is shown:
- **Quota cards** show what is *left*, with a countdown and the reset time:
  `Resets in 02:13:41 | 14:09` for the 5-hour window,
  `Resets in 03:04:59:59 | Oct 28` (days:hours:minutes:seconds) for the
  weekly one. The board counts down on its own from the reset time the PC
  sends.
- **Non-Latin text** (Japanese song titles, artist names) is rendered on
  the PC with Windows' fonts and font fallback, then sent to the board as a
  4-bit alpha bitmap. The board caches the last 8 bitmaps. The weather place
  name is pinned and saved to flash (`/ffat/txt_wx.place.bin`), so it still
  draws while the PC is off.
- **Cover art** is 144x144, sent as RGB444 (31 KB per cover). It shrinks to
  104x104 while a warning box sits above it.
- **Weather warnings:** rain within 60 min puts a box on the media and weather
  faces (Drizzle under 0.5 mm/h / Light Rain / Heavy Shower from 7.6 mm/h /
  Storm, the ETA counting down each minute, how long it lasts); yellow until
  20 min out, then red. Typhoons and super typhoons tracked from GDACS get a
  red box in their last 6 h and while they are here. Light rain that only
  one source sees is a "watch": shown on the faces, never posted.
- **PC usage warnings** take that box's place on the media face while the PC
  is on: RAM above 80% (yellow) or 90% (red), CPU, GPU load or RX 6600 VRAM
  above 90% (yellow), and CPU, GPU, RAM or VRM temperature too high (red),
  each a 2 s average. The matching PC-face cards turn yellow or red too.
  Limits and the window are set with the `pcwarn` console command and kept in
  NVS.
