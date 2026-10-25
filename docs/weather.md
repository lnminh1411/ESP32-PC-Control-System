# Weather & Rain Nowcasting

Firmware integration for live weather, advection-based rain nowcasting, OwnTracks phone location tracking, and mobile rain alerts (Discord + Pushover).

Runs on the carrier firmware (ESP-IDF 5.5) in its own FreeRTOS task.

---

## 1. Overview & Architecture

- **Home Weather Display**: Displayed on Screen 2 (Media face) whenever the host PC is off, asleep, or restarting. Shows current temperature, min/max, weather icon, wind speed/direction, US AQI, and a 3-day forecast with daily highs/lows.
- **Dedicated Network Task**: All network transactions (Open-Meteo HTTP requests, Pushover HTTPS POSTs, MQTT processing) run in a dedicated FreeRTOS task on Core 0 (`stack = 12 KB`, 48 KB PSRAM buffer), never blocking the main loop or display rendering.
- **Thread Safety**: Finished structs (`WeatherNow`, `RainAlert`) are published to the main loop under mutex. When changed, `viewsMarkDirty(-1)` is triggered from `weatherPoll()` on the main loop.
- **SNTP Synchronization**: Once Wi-Fi connects, `board_time.cpp` starts `esp_sntp` pointing to `pool.ntp.org`. Time from the PC stats service (`pc.epoch`) only overrides time if SNTP has never synced. Forecast `utc_offset_seconds` automatically sets local timezone offset if PC stats has not provided `pc.tz_min`.

---

## 2. Setting Home Location

Set the home coordinates via serial or Discord console:

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
   - **Host**: Your HiveMQ cluster hostname (e.g. `xxxx.s1.eu.hivemq.cloud`)
   - **Port**: `8883`
   - **WebSockets**: Disabled (native TCP/TLS MQTT)
   - **TLS / SSL**: Enabled
2. **Identification**:
   - **Username**: HiveMQ cluster username
   - **Password**: HiveMQ cluster password
   - **Device ID**: (e.g. `phone`)
   - **Tracker ID**: (e.g. `p1`)
3. **Topic**:
   - Publishes to `owntracks/<username>/<device>`
   - The ESP32 subscribes to `OWNTRACKS_TOPIC` (default `owntracks/#`).
4. **Payload format**:
   - Reads JSON location events with `"_type": "location"` containing `lat`, `lon`, `tst`, `acc`.
   - Location is considered valid while `tst` timestamp is `< 2 h` old.

---

## 4. HiveMQ Cloud Free-Tier Setup

HiveMQ Cloud provides a free serverless MQTT broker suitable for OwnTracks:

1. Sign up at [HiveMQ Cloud](https://www.hivemq.com/cloud/).
2. Create a **Serverless** cluster (Free, up to 100 client connections).
3. Under **Cluster Overview**, copy your cluster URL:
   `mqtts://<cluster-id>.s1.eu.hivemq.cloud:8883`
4. Under **Access Management**, add an MQTT user credentials (username and password).
5. Add these credentials to your `secrets.h`:
   ```c
   #define MQTT_URI        "mqtts://<cluster-id>.s1.eu.hivemq.cloud:8883"
   #define MQTT_USER       "your_username"
   #define MQTT_PASS       "your_password"
   #define OWNTRACKS_TOPIC "owntracks/#"
   ```
6. Leaving `MQTT_URI` empty (`""`) keeps the phone engine disabled. Secrets are never logged to console or UART.

---

## 5. Pushover Notification Setup

Pushover sends push notifications directly to your phone when rain is heading towards your current location:

1. Create an account at [Pushover](https://pushover.net/) and install the mobile app.
2. Copy your **User Key** (`PUSHOVER_USER`).
3. Under **Your Applications**, click **Create an Application / API Token** (e.g. name "Desk Controller Rain Alert").
4. Copy your **API Token** (`PUSHOVER_TOKEN`).
5. Add keys to `secrets.h`:
   ```c
   #define PUSHOVER_TOKEN  "your_pushover_app_token"
   #define PUSHOVER_USER   "your_pushover_user_key"
   ```
6. Empty strings disable Pushover without errors.

---

## 6. Engine Merging & Priority

The firmware dynamically balances Home and Phone nowcasting:

- **Haversine Distance**: Calculates distance between `(homeLat, homeLon)` and `(phoneLat, phoneLon)`.
- **Merged (Distance < 1 km or Phone Engine Off)**:
  - Phone is considered at home.
  - Only the **HOME** engine runs.
  - Home engine receives 100% of the nowcasting call budget.
  - No away phone rain notifications are sent.
- **Away (Distance >= 1 km)**:
  - Two independent nowcast engines run: **HOME** and **PHONE**.
  - **Phone Priority**: Phone engine gets 65% of the nowcasting call budget, Home engine gets 35%.
  - When the phone engine detects rain arriving within 60 min (or raining now), it sends **one alert** per rain event:
    - Discord: `discordNotify("Rain at your location in ~N min (X mm/h, from NW)")`
    - Pushover: POST to `https://api.pushover.net/1/messages.json` with title "Rain soon".
  - **Re-arm**: The phone rain alert re-arms only after 60 minutes of continuously dry conditions.

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
https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&minutely_15=precipitation&past_minutely_15=2&forecast_minutely_15=8
```
`minutely_15` returns precipitation per 15 min in mm ($\times 4 = \text{mm/h}$) for steps $k = -2..7$ (10 steps total).

### Advection Tracking Algorithm:
1. **Precipitation Field**: $F[k][\text{point}]$ stored in mm/h for $k = -2..7$.
2. **Motion Vector ($v$)**:
   - Searches 41 candidate velocity vectors: $v = 0$ plus 8 compass directions $\times$ 5 speeds $\{10, 20, 30, 45, 60\}$ km/h.
   - For each candidate, computes Mean Squared Error across all 17 points and $k = 0..3$:
     $$\text{Error} = \sum_{p=0}^{16} \sum_{k=0}^{3} \left( F[k+1](x_p) - F[k](x_p - v \cdot 15\text{min}) \right)^2$$
   - Field values at shifted coordinates are computed using 2D inverse-distance power 2 interpolation from the 17 points.
   - The candidate with the smallest error is selected. If the entire grid is dry, $v = 0$.
   - Direction: `from` is the compass direction of $-v$ (the direction the rain approaches from).
3. **ETA & Intensity**:
   - Evaluates steps $t \in [0, 90]$ min in 5-minute increments.
   - At each $t$, checks combined rain rate: $\max(\text{center forecast}(t), F[0](\text{center} - v \cdot t))$.
   - ETA is the earliest $t$ where combined rain rate exceeds $0.5$ mm/h.
   - Intensity is the maximum rate over the 30-minute window starting at ETA.
4. **RainAlert State**:
   - Active when $\text{ETA} \le 60$ min (or raining now: ETA 0).
   - Inactive: `dryKm = 25` when all 17 points are dry for $k = 0..7$, else 0.
5. **Adaptive Cadence**:
   - **All dry**: 30 minutes
   - **Rain anywhere in grid**: 10 minutes
   - **Alert active**: 5 minutes

---

## 8. Call Budget Management

Open-Meteo allows up to 10,000 calls per UTC day. The firmware enforces a conservative hard cap of **9,500 calls/day**.

- **Call Accounting**:
  - Home forecast: 1 call
  - Home air quality: 1 call
  - Nowcast poll (multi-location 17 points): 17 calls
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

## 9. Console Commands

Access commands via USB-UART or Discord `/actions` command:

| Command | Description |
|---|---|
| `weather show` | Prints current weather, home engine status, phone engine status (distance, age, alert), and budget usage. |
| `weather home <lat>,<lon> [name]` | Sets home coordinates and location name. Persists to NVS and triggers immediate poll. |
| `weather demo [rain]` | Loads sample mock weather onto the display (with or without rain incoming alert). |
| `weather clear` | Clears all weather and rain alert buffers. |
| `weather test-alert` | Sends an immediate test notification through both Discord and Pushover channels. |

---

## 10. Sample `weather show` Output

```
weather: Hanoi 28.5 C (32/24) wind 14 km/h from 225 deg, AQI 45 | rain incoming
weather: home engine: rain in ~20 min (3.2 mm/h, from NW) | cadence 5 min
weather: phone engine: away (dist 14.20 km, age 45 s) | clear, cadence 10 min
weather: budget: 188/9500 calls today (home: 136, phone: 51, forecast: 1)
```
