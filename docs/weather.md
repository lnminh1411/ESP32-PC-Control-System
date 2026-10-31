# Weather & Rain Nowcasting

Firmware integration for live weather, advection-based rain nowcasting, tiered rain and typhoon warnings (GDACS cyclone tracking), OwnTracks phone location tracking, and mobile rain alerts (Discord + Pushover).

Runs on the carrier firmware (ESP-IDF 5.5) in its own FreeRTOS task.

---

## 1. Overview & Architecture

- **Home Weather Display**: Displayed on Screen 2 (Media face) whenever the host PC is off, asleep, or restarting. Shows current temperature, min/max, weather icon, wind speed/direction, US AQI, and a 3-day forecast with daily highs/lows.
- **Dedicated Network Task**: All network transactions (Open-Meteo HTTP requests, Pushover HTTPS POSTs, MQTT processing) run in a dedicated FreeRTOS task on Core 0 (`stack = 9 KB`, 48 KB PSRAM buffer), never blocking the main loop or display rendering.
- **Thread Safety**: Finished structs (`WeatherNow`, `RainAlert`) are published to the main loop under mutex. When changed, `viewsMarkDirty(-1)` is triggered from `weatherPoll()` on the main loop.
- **Two sources**: Open-Meteo (forecast, AQI, the 17-point nowcast) and, with `OWM_API_KEY` set, OpenWeather One Call 4.0's minute-by-minute rain for the next hour (section 11b). Rain alerts use both.
- **SNTP Synchronization**: Once Wi-Fi connects, `board_time.cpp` starts `esp_sntp` pointing to `pool.ntp.org`. Time from the PC stats service (`pc.epoch`) only overrides time if SNTP has never synced. Forecast `utc_offset_seconds` automatically sets local timezone offset if PC stats has not provided `pc.tz_min`.

---

## 2. Setting Home Location

Put it in `main/settings.h` (applied at every boot):

```c
#define WEATHER_HOME "21.0285,105.8542,Home"   // "lat,lon" or "lat,lon,name"
```

or set it at runtime via serial or Discord console (saved in NVS; a
non-empty `WEATHER_HOME` replaces it at the next boot):

```
weather home <lat>,<lon> [name]
```

### Examples:
- Default name ("Home"):
  ```
  weather home 21.0285,105.8542
  ```
- Custom name:
  ```
  weather home 21.0285,105.8542 Hanoi
  ```
- Multi-word name (use underscores for spaces):
  ```
  weather home 37.7749,-122.4194 San_Francisco
  ```

Coordinates and name are persisted to NVS under namespace `"wx"` (`home_lat`, `home_lon`, `home_name`). Setting home triggers an immediate forecast and nowcast poll.

---

## 3. OwnTracks Phone Location Setup

OwnTracks sends GPS updates from your smartphone to your private MQTT broker.

### App Configuration:
1. **Preferences -> Connection**:
   - **Mode**: MQTT
   - **Host**: your EMQX Serverless deployment address (e.g. `xxxxxxxx.ala.asia-southeast1.emqxsl.com`)
   - **Port**: `8883`
   - **WebSockets**: Disabled (native TCP/TLS MQTT)
   - **TLS / SSL**: Enabled
2. **Identification**:
   - **Username**: an EMQX client-authentication user (the phone's own, or shared with the board)
   - **Password**: that user's password
   - **Device ID**: (e.g. `phone`)
   - **Tracker ID**: (e.g. `p1`)
3. **Topic**:
   - Publishes to `owntracks/<username>/<device>`
   - Set `OWNTRACKS_TOPIC` to that exact topic. With a wildcard (`owntracks/#`, the fallback when it is empty) the board also receives events, waypoints and other devices, and logs a hint.
   - Turn on **Retain** in the app, so the broker hands the last location to the board right after it connects.
4. **Payload format**:
   - Reads JSON location events with `"_type": "location"` containing `lat`, `lon`, `tst`, `acc`.
   - Location is considered valid while `tst` timestamp is `< 12 h` old (`OWNTRACKS_MAX_AGE_S`): the app reports on movement, so no report means the phone has not moved.
   - After 2 h without a report (`OWNTRACKS_ASK_AFTER_S`) the board publishes OwnTracks' remote command `{"_type":"cmd","action":"reportLocation"}` to `<device topic>/cmd` (retained, cleared by the next fix), again every 2 h until a fix arrives. It never asks while the last fix is within `OWNTRACKS_HOME_KM` (1 km) of home: the phone reports by itself once it moves away. Turn on remote commands in the app (Android: Preferences -> Advanced -> Remote commands). Android answers while connected; iOS only when the app is awake.

---

## 4. EMQX Cloud Serverless Setup

EMQX Cloud's Serverless broker is free up to a monthly allowance of session minutes and traffic. One always-connected board is about 44,600 minutes a month, and OwnTracks messages are tiny, so the board and a phone fit with room to spare.

1. Sign in at [EMQX Cloud](https://cloud-intl.emqx.com/) and open the project (here **ESP32 Control**).
2. **EMQX Broker -> New**, then **Serverless**. Pick the region nearest the board, e.g. Singapore (`ap-southeast-1`) from Vietnam, and keep the free spend limit.
3. On the deployment's **Overview**, copy the **Address** and the **MQTT over TLS/SSL port** (8883).
4. **Access Control -> Client Authentication -> Add**: one username and password, or one each for the board and the phone.
5. The server certificate chains to DigiCert Global Root G2. It is in ESP-IDF's full certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`), so no CA file is needed on the board.
6. Add the credentials to your `settings.h`:
   ```c
   #define MQTT_URI        "mqtts://<address>:8883"
   #define MQTT_USER       "your_username"
   #define MQTT_PASS       "your_password"
   #define OWNTRACKS_TOPIC "owntracks/your_username/phone"
   ```
7. Leaving `MQTT_URI` empty (`""`) keeps the phone engine disabled. Secrets are never logged to console or UART.

**Client footprint** (`owntracks.cpp`):
- TLS over TCP only. The WebSocket transports are off in sdkconfig.defaults.
- Keepalive 120 s.
- Task stack 4096 B (default 6144). It peaked at 2.85 KB, TLS handshake included (`bench usage`, EMQX, 2026-10-27).
- A bare broker address in `MQTT_URI` (as the EMQX console shows it) means TLS on port 8883.
- 512 B receive buffer and 256 B send buffer (the reportLocation command is about 130 B). A location is about 300-450 B. A bigger message arrives in pieces and is joined in a temporary heap copy, up to 4 KB.

---

## 5. Pushover Notification Setup

Pushover sends every #pc-announcement and #weather-announcement post to your phone as a push notification: rain heading your way, weather alerts, cyclones, PC on/off, night mode, board restarts. The Discord title becomes the notification title, and the embed text the message (or the title when there is none). Posts are queued (6 at most) and sent one per second by the weather task. Weather posts are queued for Pushover directly, so they reach the phone even without a Discord token.

Priority: a weather warning that needs you (rain's first warning, the rain starting, the tier going up, the 20-min heads-up, a typhoon's first and imminent posts, OpenWeather alerts; `weather test-alert` too) goes out high (Pushover priority 1, `WEATHER_ALERT_PRIO`: rings through quiet hours). Follow-ups (a start or end moving 20+ min, at most one per 30 min), "Cleared" posts and tropical-storm posts are normal (0) by day and quiet (-1) from 22:00 to 06:00. While the phone is away, home's warnings go out quiet (-1). The 2-hourly "Currently" updates are for where you are: while the phone (OwnTracks, a fix under 12 h old) is away from home, one extra Open-Meteo call fetches the weather at its location ("Currently where you are (10 km from home)", with the phone engine's rain), else home's. They go out quiet (-1: no sound or vibration).

1. Create an account at [Pushover](https://pushover.net/) and install the mobile app.
2. Copy your **User Key** (`PUSHOVER_USER`).
3. Under **Your Applications**, click **Create an Application / API Token** (e.g. name "Desk Controller Rain Alert").
4. Copy your **API Token** (`PUSHOVER_TOKEN`).
5. Add keys to `settings.h`:
   ```c
   #define PUSHOVER_TOKEN  "your_pushover_app_token"
   #define PUSHOVER_USER   "your_pushover_user_key"
   ```
6. Empty strings disable Pushover without errors.

---

## 6. Engine Merging & Priority

The firmware dynamically balances Home and Phone nowcasting:

- **Haversine Distance**: Calculates distance between `(homeLat, homeLon)` and `(phoneLat, phoneLon)`.
- **A margin, not a line**: away once the phone is 1.5 km out (`OWNTRACKS_AWAY_KM`), home
  again under 1 km (`OWNTRACKS_HOME_KM`); between the two the last answer stands. A fix
  vaguer than 500 m (`OWNTRACKS_SWITCH_ACC_M`) switches neither way.
- **Merged (Distance < 1 km or Phone Engine Off)**:
  - Phone is considered at home.
  - Only the **HOME** engine runs.
  - Home engine receives 100% of the nowcasting call budget.
  - No away phone rain notifications are sent.
- **Away (Distance >= 1 km)**:
  - Two independent nowcast engines run: **HOME** and **PHONE**.
  - **Phone Priority**: Phone engine gets 65% of the nowcasting call budget, Home engine gets 35%.
  - **Warnings follow the phone**: the full warning set of section 8 (tier, ETA, start,
    end, cleared) runs for the phone's place, titled "where you are", at
    `WEATHER_ALERT_PRIO`. Home's warnings still post, at Pushover priority -1 (quiet),
    until the phone is back within 1 km; then home's warnings take over again, and a home
    warning still out is posted once more, ringing.
  - The 2-hourly update (section 10) is for the phone's place too.
- **Fix age**: a phone fix counts for 12 h (`OWNTRACKS_MAX_AGE_S`). OpenWeather (section
  11b) follows the phone on the same rule.

---

## 7. Nowcast Engine & Advection Tracking

The nowcast engine runs for each active location (Home and Phone).

### 17-Point Ring Grid
In a single multi-location query to Open-Meteo, 17 coordinates are sampled:
- **Point 0**: Center location `(0, 0)`
- **Points 1–8**: 8 compass points (N, NE, E, SE, S, SW, W, NW) at **10 km** radius
- **Points 9–16**: 8 compass points (N, NE, E, SE, S, SW, W, NW) at **25 km** radius

Endpoint:
```
https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&minutely_15=precipitation&past_minutely_15=2&forecast_minutely_15=8&timeformat=unixtime
```
`minutely_15` returns precipitation per 15 min in mm ($\times 4 = \text{mm/h}$) for steps $k = -2..7$ (10 steps total).
Each value is the rain of the 15 minutes *before* its timestamp, so the engine
takes it as the rain at timestamp − 7.5 min.

Outside Central Europe and North America, Open-Meteo interpolates these
15-minute steps from hourly models: in Hanoi the data changes about once an
hour. Polling faster than every 5 minutes buys nothing, which is why the
countdowns below run from the clock instead.

### Advection Tracking Algorithm:
1. **Precipitation Field**: $F[k][\text{point}]$ stored in mm/h for $k = -2..7$.
2. **Motion Vector ($v$)**:
   - Searches 41 candidate velocity vectors: $v = 0$ plus 8 compass directions $\times$ 5 speeds $\{10, 20, 30, 45, 60\}$ km/h.
   - For each candidate, computes Mean Squared Error across all 17 points and $k = 0..3$:
     $$\text{Error} = \sum_{p=0}^{16} \sum_{k=0}^{3} \left( F[k+1](x_p) - F[k](x_p - v \cdot 15\text{min}) \right)^2$$
   - Field values at shifted coordinates are computed using 2D inverse-distance power 2 interpolation from the 17 points.
   - The candidate with the smallest error is selected. If the entire grid is dry, $v = 0$.
   - Direction: `from` is the compass direction of $-v$ (the direction the rain approaches from).
3. **Start, End & Intensity**:
   - Time $t$ counts from slot $k = 0$'s midpoint; the search starts at *now*
     (from the slot timestamps and the clock) and runs to $t = 105$ min in 5-minute steps.
   - At each $t$, checks combined rain rate: $\max(\text{center forecast}(t), F[0](\text{center} - v \cdot t))$.
   - Start is the earliest $t$ where the combined rate exceeds $0.3$ mm/h
     (`NOWCAST_RAIN_MMH`; Open-Meteo's smallest step is 0.4 mm/h, so this catches
     drizzle); end is the first step after it under $0.1$ mm/h (`NOWCAST_RAIN_END_MMH`).
   - Past the 105-min horizon the end comes from the hourly forecast: the first hour under 0.2 mm.
   - Intensity is the maximum rate between start and end.
4. **RainAlert State**: absolute `startUnix` / `endUnix`, so the ETA and the time left count
   down every minute between polls (`rainEtaMin()`, `rainLeftMin()`).
   - `active` (a warning) when rain starts within 60 min; `raining` when it rains now.
   - Inactive: `dryKm = 25` when all 17 points are dry for $k = 0..7$, else 0.
5. **Adaptive Cadence**:
   - **All dry**: 30 minutes
   - **Rain anywhere in grid**: 10 minutes
   - **Warning or raining**: 5 minutes (catches an hourly model update soon after it lands)

---

## 8. Warnings

Six tiers, mildest first:

| Tier | When |
|---|---|
| Drizzle | under 0.5 mm/h (`WEATHER_LIGHT_RAIN_MMH`) |
| Light Rain | 0.5 to 7.6 mm/h |
| Heavy Shower | 7.6 mm/h or more (nowcast or hourly forecast) |
| Storm | thunder (WMO 95-99) or gusts of 62+ km/h (Beaufort 8) in the hourly forecast while it rains |
| Typhoon | a GDACS cyclone of typhoon strength whose centre comes within 300 km |
| Super Typhoon | the same, with a peak wind of 184+ km/h (Vietnam's *siêu bão*, level 16) |

Rain (home engine) to #weather-announcement:

- **On its way** (starts within 60 min): a post when the warning starts, then only when the
  ETA drops to 20 min or less (one red heads-up), the tier goes up, or the start time moves
  20+ min (a quiet follow-up, at most one per 30 min). A plain countdown posts nothing.
  Title `Heavy Shower in ~25 min | <place>`, with how long it lasts and when it ends, the
  intensity, and who sees it (`Seen by OpenWeather only (Open-Meteo: dry)`).
  Yellow while more than 20 min out, red from 20 min.
- **Raining**: one red post `Heavy Shower now | <place>` with the expected end, then a post
  when the tier goes up (ringing) or that end moves 20+ min (quiet follow-up). The display
  shows no warning box while it rains; the weather face's rain box shows the end time instead.
- **Cleared**: 20 min with neither a warning nor a watch after one: green
  `Weather Warning Cleared`. A source going blank for a poll or two doesn't clear and re-warn.
- **Restarts**: the warning state is kept in NVS (`wx`/`warn`) for 3 h, so a reboot doesn't
  post the same warning again.
- **At night** (20:00-04:00) rain warns like by day: drizzle too.
- **Watch**: rain only one source sees, and not convincingly (see 11b), is shown on the
  weather face (`Rain possible ~19:31`, `OpenWeather only, 0.6 mm/h`), in `weather show`
  and in the "Currently" update, but never posted as a warning.
- **Length**: see 8b. Posts name where the end comes from: `Lasts ~35 min (until ~14:40, upwind)`.

### 8b. Rain end

No source sees a rain's end reliably more than about an hour out. The end is taken, first
that has one:

1. **Nowcast and OpenWeather**: Open-Meteo's nowcast end (about 105 min ahead) and
   OpenWeather's minute-by-minute end (inside its hour, with 10+ dry minutes after). The
   OpenWeather end counts only once two of its hours in a row agree on it within 5 min.
   With both, the later one.
2. **Upwind probe** (home only): while rain is coming or falling and the nowcast found it
   moving (5+ km/h), every 10 min (`WEATHER_UPWIND_POLL_MS`) one Open-Meteo call for 8
   points along the motion vector, where the rain that reaches home in 10, 20, 30, 45, 60,
   75, 90 and 120 min is now. Each point's current 15-min value is carried home unchanged
   at the rain's speed; the end is halfway between the last wet point and the first dry one
   after it. 8 calls each time, counted in the home budget. Open-Meteo's 15-min field
   outside Europe and North America is model data, not radar, so this reads the model's
   rain band further upstream than the 25 km nowcast grid.
3. **Hourly forecast**: the first dry hour 30+ min after the start, only when the hourly
   forecast has the rain at all.
4. **Usual length** when nothing has an end, by the kind of rain in the hourly weather
   codes around the start: a shower (80-82, or rain the forecast misses altogether)
   `usually 20-60 min (until ~14:20-15:00)`, a thunderstorm (95-99) 30-90 min
   (`WEATHER_SHOWER_*`, `WEATHER_TSTORM_*`), steady rain (51-67) `no end in the 12 h forecast`.

**Accuracy tracking**: for each rain at home, the board keeps every source's first end
estimate (nowcast, OpenWeather, upwind, hourly, and the middle of the usual range) and
scores it against when the rain was seen to stop: the first poll in which no source sees
rain at that minute, once it has stayed so for 10 min. Per source it keeps the number of
rains, the mean minutes off, the bias (+ = guessed too late) and how far ahead the first
guess came, in NVS (`endst`), so they survive reboots. `weather show` prints them and the
last rain's errors; `weather end-reset` clears them. Each finished rain also logs
`weather: rain 14:02-14:47 over; first end guesses off by (min): nowcast +5, upwind -3`.

Typhoons:

- Tracked: typhoons and tropical storms (class TS/STS, or 63+ km/h). A tropical storm
  posts at follow-up priority (normal by day, quiet at night) and shows as
  `TROPICAL STORM` on the display.
- More than 6 h out: one yellow post per GDACS advisory (about every 6 h) with the
  arrival time, closest approach and peak wind.
- Last 6 h: a post on entering it, one when it turns imminent, else every 30 min or when
  the arrival moves 30+ min; red warning box on the display.
- Here (centre within 300 km): red box `TYPHOON <NAME> ACTIVE`, a post every hour. Rain posts
  pause meanwhile.
- Gone: green `Weather Warning Cleared`.

## 9. Typhoon Tracking (GDACS)

[GDACS](https://www.gdacs.org) (UN / EU Joint Research Centre, no key) publishes every
tropical cyclone with its JTWC track and forecast:

- Event list (`geteventlist/SEARCH?eventlist=TC`, ~6 KB) every 30 min. Storms within
  3000 km of home are tracked, up to three.
- Each tracked storm's geometry (`polygons/getgeometry`, ~350 KB) is fetched into a 1 MB
  PSRAM buffer, only when GDACS publishes a new advisory (episode) for it. Both are
  scanned as text: cJSON would need megabytes, and its nodes would come out of internal RAM.
- Track points (time, position) come from the `Point_Polygon_Point_N` markers (bbox centre,
  `key` = MMDDhhmm UTC); the class per point (TD, TS, HU) from the `Line_Line_N` segments.
- Every 30 s the board places each storm on its track at the current time, walks the
  forecast in 15-min steps for the first time within 300 km and the closest approach,
  and keeps the most pressing storm.
- Limits: GDACS gives the storm's *peak* wind, not the wind at arrival, so "super"
  can overstate a storm that has weakened; the forecast reaches 3-5 days ahead.

## 10. Weather Updates

Every 2 h from 04:00 to 20:00 local (`WEATHER_SUMMARY_FIRST_HOUR`,
`WEATHER_SUMMARY_LAST_HOUR`), `Currently in <place> | HH:MM` goes to
#weather-announcement: temperature, today's range, conditions, wind, AQI, rain status and
any tracked typhoon. While the phone is away (section 6) one extra Open-Meteo call gets
the weather at its location instead: `Currently where you are (N km from home) | HH:MM`.
The 04:00 one (`WEATHER_MORNING_HOUR`) adds the 3-day forecast. The posted slot is kept in NVS
(`upd_slot`) so a reboot in the same hour doesn't repeat it.

---

## 11. Call Budget Management

Open-Meteo allows up to 10,000 calls per UTC day. The firmware enforces a conservative hard cap of **9,500 calls/day**.

- **Call Accounting**:
  - Home forecast (with 12 hourly steps): 1 call, every 15 min (96/day; the data changes hourly)
  - Home air quality: 1 call, every 30 min (48/day)
  - Nowcast poll (multi-location 17 points): 17 calls
  - GDACS (cyclones) is a separate service and not counted
- **Daily Allocation**:
  - Reserved for Forecast + AQI: 400 calls/day
  - Available for Nowcast engines: $9,500 - 400 = 9,100$ calls/day
- **Engine Shares**:
  - Merged: Home 100%, Phone 0%
  - Away: Phone 65%, Home 35%
- **Pacing Rule**:
  Before each 17-call nowcast poll, the poll is allowed only if:
  $$\text{used\_by\_engine} + 17 \le \text{share} \times (\text{elapsed\_fraction\_of\_day} \times 9100 + 300)$$
  where $\text{elapsed\_fraction\_of\_day} = \frac{\text{seconds elapsed in UTC day}}{86400}$.
  If budget is exceeded, the poll is throttled and retried at the next cadence tick. A warning is logged once per engine when throttled.
- **NVS Persistence**:
  Budget counters (`b_day`, `b_tot`, `b_home`, `b_phone`, `b_fc`) are persisted to NVS every 15 minutes and restored across reboots.

---

## 11b. OpenWeather One Call 4.0

A second rain source next to Open-Meteo: `timeline/1min` gives precipitation (mm/h) for each of the next 60 minutes, measured for the exact place, plus the IDs of the weather alerts in force there (`firmware/main/owm.cpp`).

**Setup**

1. Create an account at openweathermap.org and subscribe to **One Call API 4.0** ("One Call by Call"). A One Call 3.0 subscription does not carry over, and 3.0 can no longer be subscribed to.
2. In the account's billing settings, cap the daily calls at **1000** (the free tier), so OpenWeather itself never bills for more.
3. Copy the API key into settings.h: `#define OWM_API_KEY "..."`. Empty = Open-Meteo only.

**Which place**: the phone while it is away (section 6) with a fix under 12 h old; home otherwise. The other place keeps Open-Meteo alone. A move switches right away (one call).

**Cadence and budget**: one call every 2 min; every 90 s while rain is expected or falling at that place, or a weather alert is in force. At most **960 calls per UTC day**, counted on the board (NVS `o_day`, `o_calls`, saved every 10 calls and with the Open-Meteo counters). Paced: no more than the day's share so far plus 60, so a rainy stretch can't spend the rest of the day's calls (the UTC day ends at 07:00 local, right after an early-morning shower). At 960 it stops until 00:00 UTC.

**Empty answers**: OpenWeather sometimes answers 200 with no minutes (`"data": []`) for a few minutes at a time. A failed answer is retried once after 30 s, and for up to 15 min the last good hour still counts while its minutes reach 30 min ahead, so a blank spell doesn't read as dry. A rejected key (401) pauses it for 30 min, a rate limit (429) for 15. The key is in the URL, so URLs are never logged.

**Merging** (`owmRainSummary` in owm.cpp, `mergeRain` in weather.cpp): rain counts from 0.3 mm/h (`OWM_RAIN_MMH`), Open-Meteo's nowcast threshold. A source counts as *fresh and dry* when its last answer saw no rain: OpenWeather for 5 min after a fetch (its 60 minutes), Open-Meteo for 20 min after a nowcast (its next 60 minutes).

- **Both see rain**: a warning. OpenWeather's start time wins, the intensity is the higher of the two.
- **One sees rain** (thresholds in settings.h):
  - 2 mm/h or more (`WEATHER_RAIN_SURE_MMH`): a warning, whatever the other says.
  - OpenWeather alone with a peak under 0.8 mm/h and fewer than 3 rain minutes in a row (`OWM_RAIN_SOLID_*`): a watch.
  - The other source fresh and dry over that time: a watch; a warning only once it rains and the lone source has seen it for 3 polls in a row.
  - Otherwise: a warning when it starts within 30 min (`WEATHER_RAIN_SURE_MIN`) or the lone source has seen it for 2 polls in a row; a watch before that.
- **Neither**: dry. "Cleared" follows two polls without a warning.
- The end: see 8b (OpenWeather's counts once two hours in a row agree on it).
- During a watch OpenWeather polls at the rain cadence, to confirm or drop it sooner.

Discord rain posts and the media screen's rain box say who saw it: Open-Meteo, OpenWeather, both, or the radar (11c).

**Weather alerts**: a new alert ID gets one `alert/{id}` call (one per poll at most) and a red post in #weather-announcement: the event, the issuer, start and end, and the first 200 characters of the text.

## 11c. RainViewer radar

Both forecasts are models: in Hanoi Open-Meteo's 15-min data is interpolated from hourly
runs, and on 10-29 neither saw the drizzle at 05:00. The radar sees rain that is falling
(`firmware/main/radar.cpp`; free, no key, `RADAR_ENABLED 0` turns it off).

- **Place**: the same as OpenWeather's (the phone while away, else home).
- **Fetch**: `api.rainviewer.com/public/weather-maps.json` for the newest frame (every
  10 min, about 10 min late), then one 256 px tile centred on the place
  (`/256/7/<lat>/<lon>/2/0_0.png`: zoom 7, ~1.1 km a pixel, ~140 km across, Universal Blue,
  no smoothing). The PNG is decoded with LovyanGFX's pngle (~44 KB, PSRAM) and each pixel's
  colour mapped back to dBZ through the scheme's table.
- **Rain here**: 15 dBZ or more (`RADAR_RAIN_DBZ`, ~0.3 mm/h) on 3+ pixels
  (`RADAR_HERE_MIN_PX`) within 2.5 km (`RADAR_HERE_KM`). Rain rate from Marshall-Palmer
  (Z = 200 R^1.6).
- **Cadence**: every 10 min (`RADAR_POLL_DRY_MS`), every 5 while an echo is within 30 km or
  rain is near (`RADAR_POLL_MS`). A frame older than 25 min (`RADAR_MAX_AGE_S`) says nothing.
- **Merging**: rain on the radar at the place makes it *raining* whatever the models say:
  never a watch, the start at the frame's time, the intensity the higher one. The post says
  `On the radar now (the forecasts missed it)` or `..., and in Open-Meteo`. A dry radar
  doesn't cancel a model's warning (the models look ahead, the radar doesn't).
- `weather show` prints the place, the frame, the strongest echo at the place and the
  nearest echo's distance; the log notes when the place turns wet or dry on the radar.

---

## 12. Console Commands

Access commands via USB-UART or Discord `/actions` command:

| Command | Description |
|---|---|
| `weather show` | Prints current weather, home engine status (tier, start and end), phone engine status, OpenWeather (place, last hour, alerts, calls today), the radar (frame, echo here, nearest echo), which source sees the rain, tracked cyclones, and budget usage. |
| `weather refresh` | Fetches the forecast, AQI, nowcast and GDACS list again now. |
| `weather home <lat>,<lon> [name]` | Sets home coordinates and location name. Persists to NVS and triggers immediate poll. |
| `weather demo [rain\|typhoon]` | Loads sample weather onto the display for 5 min: dry, a heavy shower in 25 min, or a typhoon at home. Polls leave it alone and nothing is posted. |
| `weather clear` | Clears all weather, rain alert and cyclone buffers. |
| `weather storms` | Fetches the GDACS list and every nearby storm's track again now. |
| `weather owm-check [minute][L]` | Runs the merge on a made-up OpenWeather hour, 20 min of rain from that minute (4 mm/h; `L` = 0.6 mm/h), against home's Open-Meteo result, and prints the verdict (warning, watch, raining, dry), start, end, where the end comes from, the kind of rain and the sources. Nothing is stored or posted. |
| `weather end-reset` | Clears the rain-end accuracy stats (8b). |
| `weather test-alert` | Sends an immediate test notification through both Discord and Pushover channels. |

---

## 13. Sample `weather show` Output

```
weather: Tương Mai 28.8 C (31/25) wind 15 km/h from 116 deg, AQI 105 | rain none
weather: home engine: Light Rain now (14:47 to 17:00, 1.2 mm/h, from E) | cadence 5 min
weather: phone engine: off (MQTT_URI empty)
weather: cyclone TWO: 1971 km now, closest 1971 km, peak 120 km/h, not a typhoon threat
weather: cyclone CHAMPI: 2791 km now, closest 2791 km, peak 296 km/h, not a typhoon threat
weather: budget: 369/9500 calls today (home: 357, phone: 0, forecast: 12)
```
