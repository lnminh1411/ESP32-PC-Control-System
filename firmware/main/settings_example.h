// Secrets, then settings. Copy this file to settings.h (gitignored) and fill
// in the secrets; change any setting there too (defaults and notes are in
// settings_or_example.h). Without a settings.h the firmware still builds (CI
// does), with these placeholders, and simply never joins a network.
#pragma once

#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-wifi-password"

// Discord bot (discord.h has the setup steps). Empty token = bot off.
#define DISCORD_TOKEN      ""
#define DISCORD_APP_ID     ""   // application id
#define DISCORD_GUILD_ID   ""   // your server's id (slash commands register there)
#define DISCORD_OWNER_ID   ""   // your user id: the only account allowed to use it
#define DISCORD_CHANNEL_ID ""   // optional: default for #pc / #weather below when they are empty
// Channel ids (Discord: Settings > Advanced > Developer Mode, then right-click
// a channel > Copy Channel ID). Empty = that feature is off (or, for PC and
// weather, posts go to DISCORD_CHANNEL_ID).
#define DISCORD_CH_PC      ""   // #pc-announcement: board online, PC on/off, restart/update, power, door
#define DISCORD_CH_WEATHER ""   // #weather-announcement: rain alerts, morning forecast
#define DISCORD_CH_LOG     ""   // #logging: the board's log, posted in batches
#define DISCORD_CH_CONTROL ""   // #control: a standing control panel with buttons
// Discord webhook for the Screenshot button: a screenshot of the PC desktop
// is posted here, then auto-deleted after 5 min. Make one at #logging ->
// Edit Channel -> Integrations -> Webhooks. Empty = the button is disabled.
#define DISCORD_SHOT_WEBHOOK ""

// Password for over-the-air firmware updates (arduino-cli / Arduino IDE
// network port). Empty = OTA disabled.
#define OTA_PASS ""

// OwnTracks over MQTT (TLS, EMQX Cloud Serverless: mqtts://<address>:8883, or the bare address = TLS on 8883). Empty MQTT_URI = phone engine off.
#define MQTT_URI        ""
#define MQTT_USER       ""
#define MQTT_PASS       ""
#define OWNTRACKS_TOPIC "owntracks/<user>/<device>"   // exact topic; a wildcard also brings events and other devices

// Pushover notifications for rain alerts. Empty token/user = Pushover disabled.
#define PUSHOVER_TOKEN  ""
#define PUSHOVER_USER   ""

// Wake-on-LAN. The PC stats service reports the PC's MAC by itself; WOL_MAC
// is used until it has (fresh board, or the PC was never on with the service).
// WOL_TARGET: extra address to send the magic packet to, e.g. a router that
// forwards it ("192.168.1.255" or a public IP); empty = local broadcasts only.
#define WOL_MAC         ""   // "AA:BB:CC:DD:EE:FF"
#define WOL_TARGET      ""


// Weather home location, "lat,lon" or "lat,lon,name" (e.g. "21.0285,105.8542,Home").
// Applied at every boot; empty = use the one set with `weather home <lat>,<lon> [name]`.
#define WEATHER_HOME    ""

// OpenWeather One Call 4.0 key: minute-by-minute rain for the next hour next
// to Open-Meteo (docs/weather.md). Empty = Open-Meteo only. The board makes at
// most 960 calls a day; set the account's own daily cap to 1000 as well.
#define OWM_API_KEY     ""


// ==== settings: defaults and notes in settings_or_example.h ====

// ---- Board pins (rev 3.1 carrier; scripts/check_pins.py checks them against pcb/main.ato) ----
#define PIN_SCK 12                                // panels' shared SCK, via 33R (FSPI IO_MUX pin)
#define PIN_MOSI 11                               // panels' shared MOSI, via 33R (FSPI IO_MUX pin)
#define PIN_DC 9                                  // panels' shared DC
#define PIN_CS_LIST {8, 18, 17}                   // CS of panels 1-3
#define PIN_RST_LIST {10, 16, 15}                 // RST of panels 1-3
#define PIN_BL_LIST {3, 6, 5}                     // backlight PWM of panels 1-3
#define PIN_DOOR_LIST {39, 40, 41, 42}            // keyfob B1-B4 (opto rows 1-4)
#define PIN_MOBO_RST 1                            // motherboard RST header (opto row 5)
#define PIN_LED 2                                 // status LED (PWM)
#define ONBOARD_RGB_GPIO 48                       // the module's own WS2812 (not on the carrier; YD-style boards need the "RGB" jumper bridged), held off
#define UART_BAUD 3000000                         // host UART (CH343): console and GIF upload

// ---- Panels ----
#define N_PANELS 3                                // ST7789 panels on the bus
#define TFT_W 240                                 // panel width, px
#define TFT_H 320                                 // panel height, px
#define SPI_HZ 40000000                           // shared SPI clock; 80 MHz gave blank panels on the carrier bus
#define PANEL_MADCTL_LIST {0, 0, 0}               // ST7789 MADCTL (rotation / RGB order) of panels 1-3
#define PANEL_INVERT_LIST {true, true, true}      // colour inversion (IPS panels) of panels 1-3
#define PANEL_DEFAULT_VIEW_LIST {VIEW_METRICS, VIEW_METRICS, VIEW_METRICS}  // view of panels 1-3 until one is saved (enum View, display.h)

// ---- Status LED ----
#define LED_DUTY_CEIL 0.35f                       // green LED glares above ~35 % duty; brightness 100 maps here
#define LED_GAMMA 2.2f                            // brightness % is perceptual, not linear duty

// ---- Status LED patterns ----
#define LED_RESTART_BLINK_MS 400                  // PC restarting / updating: blink period (half on)
#define LED_LOADING_BLINK_MS 200                  // board booting / stats not fresh: blink period (half on)
#define LED_BREATHE_MS 3000                       // PC asleep / off: breathing period

// ---- Door keyfob and PC reset (optos) ----
#define DOOR_PRESS_MS 500                         // keyfob button press length
#define DOOR_GAP_MS 500                           // gap between two presses
#define MOBO_RST_MS 200                           // PC reset pulse length

// ---- PC power state ----
#define USB_GUARD_MS 30000                        // after the USB link comes up, suspends are ignored this long
#define POWER_ACK_TIMEOUT_MS 3000                 // wait for the PC helper's power.ack
#define POWER_WAKE_DIM_MS 200                     // backlights off this long while the PC powers on (its inrush browns out the board)

// ---- Network ----
#define NET_HOSTNAME "esp32-pc-control"           // Wi-Fi hostname, mDNS name (<name>.local), OTA name, Discord device name
#define NET_NTP_SERVER "pool.ntp.org"             // SNTP server
#define NET_WIFI_PS WIFI_PS_NONE                  // Wi-Fi modem power save (esp_wifi_set_ps)
#define NET_OTA_VALIDATE_MS 120000                // a new OTA image is marked good after this much uptime

// ---- Wake-on-LAN ----
#define WOL_PORT 9                                // magic packet UDP port
#define WOL_PORT_ALT 7                            // second UDP port for the subnet broadcast
#define WOL_BURSTS 3                              // magic packet rounds per wake
#define WOL_BURST_GAP_MS 100                      // gap between rounds

// ---- PC stats link ----
#define PCSTATS_FRESH_MS 3000                     // a sample newer than this counts as "PC helper talking"
#define PCSTATS_STALE_MS 5000                     // values older than this read as missing (NAN / "")
#define CONTROL_PANEL_REFRESH_MS 5000             // #control panel: edited this often while live (below), when something on it changed
#define CONTROL_PANEL_LIVE_MS (3 * 60 * 1000)     // live this long after any interaction with the bot; else every 10 min

// ---- PC warnings (pcwarn.cpp; `pcwarn set` overrides per metric in NVS) ----
#define PCWARN_AVG_MS 2000                        // averaging window before a level triggers
#define PCWARN_RAM_LOAD_HIGH 80.0f                // RAM load %, yellow
#define PCWARN_RAM_LOAD_CRIT 90.0f                // RAM load %, red
#define PCWARN_CPU_LOAD_HIGH 90.0f                // CPU load %, yellow
#define PCWARN_GPU1_LOAD_HIGH 90.0f               // GPU1 (RX 6600) load %, yellow
#define PCWARN_GPU2_LOAD_HIGH 90.0f               // GPU2 (RTX 3050) load %, yellow
#define PCWARN_GPU1_VRAM_HIGH 90.0f               // GPU1 VRAM used %, yellow
#define PCWARN_CPU_TEMP_CRIT 80.0f                // CPU temp C, red
#define PCWARN_GPU1_TEMP_CRIT 80.0f               // GPU1 temp C, red
#define PCWARN_GPU2_TEMP_CRIT 75.0f               // GPU2 temp C, red
#define PCWARN_RAM_TEMP_CRIT 75.0f                // DIMM temp C, red
#define PCWARN_VRM_TEMP_CRIT 75.0f                // VRM temp C, red

// ---- PC hardware shown on the board ----
#define PC_GPU1_LABEL "RX 6600"                   // GPU card 1 (stat keys gpu1_*: stats.ini maps them to a sensor)
#define PC_GPU2_LABEL "RTX 3050"                  // GPU card 2 (gpu2_*)
#define PC_DRIVE_LABEL "C:"                       // drive card (c_used / c_total)
#define PC_DRIVE_WARN_PCT 85.0                    // drive used %, yellow
#define PC_DRIVE_BAD_PCT 95.0                     // drive used %, red
#define LOCAL_AI_MODEL_NAME "Occamy-1.0"          // local model name on the Local AI face
#define LOCAL_AI_MODEL_FULL "Occamy-1.0 35B-A3B"  // the same, full, on the working face

// ---- PC case RGB and fans (PcHwCtl names, Discord RGB / Fan tabs) ----
#define HW_RGB_GROUPS {"all", "All"}, {"main", "Motherboard"}, {"ram", "RAM"}  // PcHwCtl RGB groups: {name, label}, ...
#define HW_FAN_TARGETS {"all", "All fans"}, {"case", "Case fans"}, {"cpu-fans", "CPU fans"}, {"rear-fan", "Rear fan"}, {"front-fans", "Front fans"}, {"gpu-amd", "RX 6600 fan"}, {"gpu-nvidia", "RTX 3050 fan"}  // fan targets {PcHwCtl key, label}; "all" and "case" are PcHwCtl's / the board's groups, gpu-* get a Zero RPM menu
#define HW_FAN_CASE_KEYS "cpu-fans", "rear-fan", "front-fans"  // what "case" means: one command per key
#define HW_FAN_SHOWN {"cpu_fans", "CPU fans"}, {"rear_fan", "Rear fan"}, {"front_fans", "Front fans"}, {"water_pump", "Water pump"}, {"gpu_amd", "RX 6600"}, {"gpu_nvidia", "RTX 3050"}  // Fan tab embed rows {state.txt key (_ for -), label}
#define HW_FAN_HOLD_MIN 5, 15, 60                 // hold times a manual fan speed cycles through, min

// ---- Local AI (LocalAIHost) ----
#define LOCAL_AI_IDLE_MIN 20                      // auto-unload minutes the Local Agent tab restores after "off"

// ---- Quota faces (Claude, Antigravity) ----
#define QUOTA_SESSION_WARN_PCT 75.0               // 5 h window used %, yellow
#define QUOTA_SESSION_BAD_PCT 90.0                // 5 h window used %, red
#define QUOTA_WEEKLY_WARN_PCT 80.0                // weekly used %, yellow
#define QUOTA_WEEKLY_BAD_PCT 95.0                 // weekly used %, red

// ---- Screens and frame rate ----
#define SCREEN_MARQUEE_PX_S 30.0f                 // media title scroll speed, px/s
#define SCREEN_MARQUEE_PAUSE_S 1.5f               // pause at each end of the title
#define VIEW_BUS_BUDGET_PCT 85                    // share of the SPI bus the panels may use (`fps budget` changes it)
#define VIEW_MIN_FRAME_MS 50                      // canvas faces: at most 20 fps
#define VIEW_GIF_MIN_FRAME_MS 16                  // GIF face: at most ~60 fps (GIF frame times still apply)
#define VIEW_METRICS_PAGE_MS 5000                 // metrics face pages through overflow cards this often

// ---- PC shutdown / restart announcements (pc_going.cpp) ----
#define PCGOING_PREOFF_MS (120 * 1000)            // after "restart/shut down" is announced, wait this long for the PC to go off
#define PCGOING_PREOFF_UPDATE_MS (60 * 60 * 1000)  // same, for an update restart / shutdown
#define PCGOING_BACK_MS (10 * 60 * 1000)          // after it went off, screens wait this long for it to come back
#define PCGOING_BACK_UPDATE_MS (2 * 60 * 60 * 1000)  // same, for an update restart (several reboots)

// ---- Night mode (board side; night.cpp) ----
#define NIGHT_START_HOUR 22                       // local hour the night window opens
#define NIGHT_END_HOUR 6                          // local hour it closes
#define NIGHT_IDLE_ENGAGE_S (15 * 60)             // s without keyboard/mouse input on the PC before it engages in the window
#define NIGHT_PANEL_PCT 10                        // board panel backlight % while engaged
#define NIGHT_STATUS_LED_PCT 20                   // status LED cap % while engaged
#define NIGHT_RGB_PCT 10                          // PC case RGB % while engaged (PcHwCtl)
#define NIGHT_GUARD_MS 10000                      // input during the night only ends it if this window shows a real return:
#define NIGHT_GUARD_PX 50                         //   mouse moved more than this many px,
#define NIGHT_GUARD_PRESSES 3                     //   or this many key / button presses
#define NIGHT_LASSO_REPEAT_MS 30000               // !night state resent to the PC this often (Process Lasso profile)

// ---- Weather (board) ----
#define WEATHER_HOME_NAME "Home"                  // place name until WEATHER_HOME or `weather home` gives one
#define WEATHER_RAIN_URGENT_MIN 20                // rain ETA at or under this: red banner / red Discord post
#define WEATHER_RAIN_SURE_MIN 30                  // rain one source alone sees: a warning at this ETA or closer (else a quiet watch)
#define WEATHER_RAIN_SURE_MMH 2.0f                // ...or at this mm/h and up, whatever the other source says
#define OWM_RAIN_SOLID_MMH 0.8f                   // OpenWeather alone: needs this peak mm/h...
#define OWM_RAIN_SOLID_MIN 3                      // ...or this many rain minutes in a row to be more than a watch
#define WEATHER_SHOWER_MIN 20                     // a shower usually lasts this many minutes...
#define WEATHER_SHOWER_MAX 60                     // ...to this many (shown when no source has an end)
#define WEATHER_TSTORM_MIN 30                     // a thunderstorm's rain: this many minutes...
#define WEATHER_TSTORM_MAX 90                     // ...to this many
#define WEATHER_UPWIND_POLL_MS (10 * 60 * 1000)   // upwind probe (8 Open-Meteo calls) this often while rain is coming or falling at home
#define WEATHER_TYPHOON_NEAR_S (6 * 3600)         // a typhoon arriving within this shows on the screens and gets per-minute posts
#define WEATHER_AQI_MODERATE 50                   // air quality index above this: Moderate
#define WEATHER_AQI_HIGH 100                      // above this: High
#define WEATHER_AQI_DANGEROUS 200                 // above this: Dangerous
#define WEATHER_TYPHOON_IMMINENT_S (20 * 60)      // under this: typhoon posts turn red
#define WEATHER_STORM_GUST_KMH 62                 // forecast gust at or over this (km/h, Beaufort 8) makes the hour a storm
#define WEATHER_HEAVY_SHOWER_MMH 7.6f             // rain at or over this mm/h is a heavy shower
#define WEATHER_SUMMARY_FIRST_HOUR 6              // two-hourly #weather summary from this hour...
#define WEATHER_SUMMARY_LAST_HOUR 20              // ...to this one (even hours)
#define WEATHER_MORNING_HOUR 6                    // the 3-day forecast post goes out at this hour
#define WEATHER_FORECAST_POLL_MS (5 * 60 * 1000)  // Open-Meteo forecast poll
#define WEATHER_AQI_POLL_MS (30 * 60 * 1000)      // air quality poll
#define WEATHER_CYCLONE_POLL_MS (30 * 60 * 1000)  // GDACS cyclone list poll

// ---- Rain nowcast (Open-Meteo 15-min grid, nowcast.cpp) ----
#define NOWCAST_RING1_KM 10.0f                    // inner sample ring radius
#define NOWCAST_RING2_KM 25.0f                    // outer sample ring radius
#define NOWCAST_RAIN_MMH 0.5f                     // over this mm/h counts as rain
#define NOWCAST_RAIN_END_MMH 0.3f                 // rain ends under this (hysteresis)
#define NOWCAST_ALERT_ETA_MIN 60                  // warn when rain arrives within this
#define NOWCAST_POLL_RAIN_MS (5 * 60 * 1000)      // poll while warned or raining
#define NOWCAST_POLL_NEAR_MS (10 * 60 * 1000)     // poll while rain is in the grid
#define NOWCAST_POLL_DRY_MS (15 * 60 * 1000)      // poll while the grid is dry

// ---- OpenWeather One Call 4.0 (owm.cpp; key: OWM_API_KEY) ----
#define OWM_CALLS_PER_DAY 960                     // daily call budget (set the account's own cap to 1000)
#define OWM_RAIN_MMH 0.5f                         // minutely precipitation over this counts as rain
#define OWM_POLL_RAIN_MS (90 * 1000)              // poll while warned, raining or under an alert
#define OWM_POLL_MS (120 * 1000)                  // poll otherwise

// ---- Tropical cyclones (GDACS, cyclone.cpp) ----
#define CYCLONE_HOME_KM 300.0f                    // centre within this distance = "at home"
#define CYCLONE_FETCH_KM 3000.0f                  // storms further away are not fetched

// ---- Phone location (OwnTracks over MQTT; broker in MQTT_URI) ----
#define OWNTRACKS_MAX_AGE_S (2 * 3600)            // a fix older than this is ignored
#define OWNTRACKS_HOME_KM 1.0                     // phone within this of home = at home (one weather engine)

// ---- PC helper (ESP32-Helper.exe; build.cmd turns HOST_* into C#, the sensor map stays in stats.ini) ----
#define HOST_LASSO_INI "C:\\ProgramData\\ProcessLasso\\config\\prolasso.ini"  // Process Lasso config the night profile rewrites (original kept as .esp32-night)
#define HOST_NIGHT_CORES "12-17"                  // cores night mode pins processes to (Core Ultra 7 265K E-cores are 2-5, 10-17)
#define HOST_NIGHT_KEEP "system,idle,registry,secure system,memory compression,smss,csrss,wininit,winlogon,services,lsass,lsaiso,fontdrvhost,dwm,audiodg,explorer,sihost,ctfmon,textinputhost,searchhost,startmenuexperiencehost,shellexperiencehost,logonui,lockapp,esp32-helper,pchwctl,localaihost,llama-server,processgovernor,processlasso,bitsumsessionagent,msmpeng,mpdefendercoreservice"  // processes night mode never pins (comma list)
#define HOST_NIGHT_NO_WORD_MIN 5                  // no !night from the board this long: the Process Lasso config goes back
#define HOST_HWCTL_PIPE "PcHwCtl"                 // named pipe of the RGB / fan service (!hw)
#define HOST_HWCTL_TIMEOUT_MS 2000                // its reply timeout
#define HOST_LOCALAI_PIPE "LocalAIHost"           // named pipe of the local model service (!ai)
#define HOST_LOCALAI_TIMEOUT_MS 20000             // its reply timeout (a forced unload takes ~8 s)
#define HOST_NIC_SKIP "hyper-v,virtualbox,vmware,tap-,loopback,bluetooth,wan miniport"  // adapters whose description contains one of these are not the PC's MAC for Wake-on-LAN
#define HOST_LHM_CPU true                         // LibreHardwareMonitor hardware groups to open
#define HOST_LHM_GPU true
#define HOST_LHM_MEMORY true
#define HOST_LHM_MOTHERBOARD true
#define HOST_LHM_STORAGE true
#define HOST_LHM_NETWORK true
#define HOST_LHM_CONTROLLER true
#define HOST_LHM_PSU true
#define HOST_LHM_BATTERY true
#define HOST_FONT_REGULAR "ComicShannsMonoNerdFontPropo-Regular.otf"  // font file for text the board can't draw (Windows or per-user Fonts folder)
#define HOST_FONT_BOLD "ComicShannsMonoNerdFontPropo-Bold.otf"
#define HOST_FONT_FALLBACK "Segoe UI"             // family when those files are missing
#define HOST_SHOT_DELETE_MIN 5.0                  // screenshot posts are deleted after this
#define HOST_SHOT_BORDER_S 5                      // red "Screenshot Captured" border shows this long
#define HOST_MON_POLL_MS 5000                     // monitor DDC/CI state feed (mon.*) period
#define HOST_MON_DDC_POWER_MODELS "DELL E1715S"   // monitor models (DDC name, comma list) that also get DDC/CI power on/off; the rest only get Windows' display off
#define HOST_MON_DDC_POWER_OFF 4                  // VCP 0xD6 value for off on those (4 = DPM off: still answers DDC, wakes on input)
