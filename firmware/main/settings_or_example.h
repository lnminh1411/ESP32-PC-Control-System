// Every setting with today's value as its default. settings.h (gitignored)
// holds the secrets and the settings; a key missing there falls back to the
// value below, so an older settings.h keeps working. Without a settings.h
// (CI) settings_example.h is used: placeholders, nothing joins a network.
// HOST_* keys are for the PC helper: host/pc_stats/gen_settings.ps1 turns
// them into C# at build time (it reads no other key, so no secret reaches
// the exe). After editing settings.h, touch this file before a build
// (ccache's direct mode once handed back a stale image).
#pragma once
#if __has_include("settings.h")
#include "settings.h"
#else
#include "settings_example.h"
#endif

// ---- secrets added after a local settings.h was made: default to off ----
#ifndef DISCORD_CH_PC
#define DISCORD_CH_PC ""
#endif
#ifndef DISCORD_CH_WEATHER
#define DISCORD_CH_WEATHER ""
#endif
#ifndef DISCORD_CH_LOG
#define DISCORD_CH_LOG ""
#endif
#ifndef DISCORD_CH_CONTROL
#define DISCORD_CH_CONTROL ""
#endif
#ifndef DISCORD_SHOT_WEBHOOK
#define DISCORD_SHOT_WEBHOOK ""
#endif
#ifndef MQTT_URI
#define MQTT_URI ""
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef OWNTRACKS_TOPIC
#define OWNTRACKS_TOPIC "owntracks/#"
#endif
#ifndef PUSHOVER_TOKEN
#define PUSHOVER_TOKEN ""
#endif
#ifndef PUSHOVER_USER
#define PUSHOVER_USER ""
#endif
#ifndef WOL_MAC
#define WOL_MAC ""
#endif
#ifndef WOL_TARGET
#define WOL_TARGET ""
#endif
#ifndef OWM_API_KEY
#define OWM_API_KEY ""
#endif
#ifndef WEATHER_HOME
#define WEATHER_HOME ""
#endif

// ---- Board pins (rev 3.1 carrier; scripts/check_pins.py checks them against pcb/main.ato) ----
#ifndef PIN_SCK
#define PIN_SCK 12                                // panels' shared SCK, via 33R (FSPI IO_MUX pin)
#endif
#ifndef PIN_MOSI
#define PIN_MOSI 11                               // panels' shared MOSI, via 33R (FSPI IO_MUX pin)
#endif
#ifndef PIN_DC
#define PIN_DC 9                                  // panels' shared DC
#endif
#ifndef PIN_CS_LIST
#define PIN_CS_LIST {8, 18, 17}                   // CS of panels 1-3
#endif
#ifndef PIN_RST_LIST
#define PIN_RST_LIST {10, 16, 15}                 // RST of panels 1-3
#endif
#ifndef PIN_BL_LIST
#define PIN_BL_LIST {3, 6, 5}                     // backlight PWM of panels 1-3
#endif
#ifndef PIN_DOOR_LIST
#define PIN_DOOR_LIST {39, 40, 41, 42}            // keyfob B1-B4 (opto rows 1-4)
#endif
#ifndef PIN_MOBO_RST
#define PIN_MOBO_RST 1                            // motherboard RST header (opto row 5)
#endif
#ifndef PIN_LED
#define PIN_LED 2                                 // status LED (PWM)
#endif
#ifndef ONBOARD_RGB_GPIO
#define ONBOARD_RGB_GPIO 48                       // the module's own WS2812 (not on the carrier; YD-style boards need the "RGB" jumper bridged), held off
#endif
#ifndef UART_BAUD
#define UART_BAUD 3000000                         // host UART (CH343): console and GIF upload
#endif

// ---- Panels ----
#ifndef N_PANELS
#define N_PANELS 3                                // ST7789 panels on the bus
#endif
#ifndef TFT_W
#define TFT_W 240                                 // panel width, px
#endif
#ifndef TFT_H
#define TFT_H 320                                 // panel height, px
#endif
#ifndef SPI_HZ
#define SPI_HZ 40000000                           // shared SPI clock; 80 MHz gave blank panels on the carrier bus
#endif
#ifndef PANEL_MADCTL_LIST
#define PANEL_MADCTL_LIST {0, 0, 0}               // ST7789 MADCTL (rotation / RGB order) of panels 1-3
#endif
#ifndef PANEL_INVERT_LIST
#define PANEL_INVERT_LIST {true, true, true}      // colour inversion (IPS panels) of panels 1-3
#endif
#ifndef PANEL_DEFAULT_VIEW_LIST
#define PANEL_DEFAULT_VIEW_LIST {VIEW_METRICS, VIEW_METRICS, VIEW_METRICS}  // view of panels 1-3 until one is saved (enum View, display.h)
#endif

// ---- Status LED ----
#ifndef LED_DUTY_CEIL
#define LED_DUTY_CEIL 0.35f                       // green LED glares above ~35 % duty; brightness 100 maps here
#endif
#ifndef LED_GAMMA
#define LED_GAMMA 2.2f                            // brightness % is perceptual, not linear duty
#endif

// ---- Status LED patterns ----
#ifndef LED_RESTART_BLINK_MS
#define LED_RESTART_BLINK_MS 400                  // PC restarting / updating: blink period (half on)
#endif
#ifndef LED_LOADING_BLINK_MS
#define LED_LOADING_BLINK_MS 200                  // board booting / stats not fresh: blink period (half on)
#endif
#ifndef LED_BREATHE_MS
#define LED_BREATHE_MS 3000                       // PC asleep / off: breathing period
#endif

// ---- Door keyfob and PC reset (optos) ----
#ifndef DOOR_PRESS_MS
#define DOOR_PRESS_MS 500                         // keyfob button press length
#endif
#ifndef DOOR_GAP_MS
#define DOOR_GAP_MS 500                           // gap between two presses
#endif
#ifndef MOBO_RST_MS
#define MOBO_RST_MS 200                           // PC reset pulse length
#endif

// ---- PC power state ----
#ifndef USB_GUARD_MS
#define USB_GUARD_MS 30000                        // after the USB link comes up, suspends are ignored this long
#endif
#ifndef POWER_ACK_TIMEOUT_MS
#define POWER_ACK_TIMEOUT_MS 3000                 // wait for the PC helper's power.ack
#endif
#ifndef POWER_WAKE_DIM_MS
#define POWER_WAKE_DIM_MS 200                     // backlights off this long while the PC powers on (its inrush browns out the board)
#endif

// ---- Network ----
#ifndef NET_HOSTNAME
#define NET_HOSTNAME "esp32-pc-control"           // Wi-Fi hostname, mDNS name (<name>.local), OTA name, Discord device name
#endif
#ifndef NET_NTP_SERVER
#define NET_NTP_SERVER "pool.ntp.org"             // SNTP server
#endif
#ifndef NET_WIFI_PS
#define NET_WIFI_PS WIFI_PS_NONE                  // Wi-Fi modem power save (esp_wifi_set_ps)
#endif
#ifndef NET_OTA_VALIDATE_MS
#define NET_OTA_VALIDATE_MS 120000                // a new OTA image is marked good after this much uptime
#endif

// ---- Wake-on-LAN ----
#ifndef WOL_PORT
#define WOL_PORT 9                                // magic packet UDP port
#endif
#ifndef WOL_PORT_ALT
#define WOL_PORT_ALT 7                            // second UDP port for the subnet broadcast
#endif
#ifndef WOL_BURSTS
#define WOL_BURSTS 3                              // magic packet rounds per wake
#endif
#ifndef WOL_BURST_GAP_MS
#define WOL_BURST_GAP_MS 100                      // gap between rounds
#endif

// ---- PC stats link ----
#ifndef PCSTATS_FRESH_MS
#define PCSTATS_FRESH_MS 3000                     // a sample newer than this counts as "PC helper talking"
#endif
#ifndef PCSTATS_STALE_MS
#define PCSTATS_STALE_MS 5000                     // values older than this read as missing (NAN / "")
#endif
#ifndef CONTROL_PANEL_REFRESH_MS
#define CONTROL_PANEL_REFRESH_MS 5000             // #control panel: edited this often while live (below), when something on it changed
#endif
#ifndef CONTROL_PANEL_LIVE_MS
#define CONTROL_PANEL_LIVE_MS (3 * 60 * 1000)     // live this long after any interaction with the bot; else every 10 min
#endif

// ---- PC warnings (pcwarn.cpp; `pcwarn set` overrides per metric in NVS) ----
#ifndef PCWARN_AVG_MS
#define PCWARN_AVG_MS 2000                        // averaging window before a level triggers
#endif
#ifndef PCWARN_RAM_LOAD_HIGH
#define PCWARN_RAM_LOAD_HIGH 80.0f                // RAM load %, yellow
#endif
#ifndef PCWARN_RAM_LOAD_CRIT
#define PCWARN_RAM_LOAD_CRIT 90.0f                // RAM load %, red
#endif
#ifndef PCWARN_CPU_LOAD_HIGH
#define PCWARN_CPU_LOAD_HIGH 90.0f                // CPU load %, yellow
#endif
#ifndef PCWARN_GPU1_LOAD_HIGH
#define PCWARN_GPU1_LOAD_HIGH 90.0f               // GPU1 (RX 6600) load %, yellow
#endif
#ifndef PCWARN_GPU2_LOAD_HIGH
#define PCWARN_GPU2_LOAD_HIGH 90.0f               // GPU2 (RTX 3050) load %, yellow
#endif
#ifndef PCWARN_GPU1_VRAM_HIGH
#define PCWARN_GPU1_VRAM_HIGH 90.0f               // GPU1 VRAM used %, yellow
#endif
#ifndef PCWARN_CPU_TEMP_CRIT
#define PCWARN_CPU_TEMP_CRIT 80.0f                // CPU temp C, red
#endif
#ifndef PCWARN_GPU1_TEMP_CRIT
#define PCWARN_GPU1_TEMP_CRIT 80.0f               // GPU1 temp C, red
#endif
#ifndef PCWARN_GPU2_TEMP_CRIT
#define PCWARN_GPU2_TEMP_CRIT 75.0f               // GPU2 temp C, red
#endif
#ifndef PCWARN_RAM_TEMP_CRIT
#define PCWARN_RAM_TEMP_CRIT 75.0f                // DIMM temp C, red
#endif
#ifndef PCWARN_VRM_TEMP_CRIT
#define PCWARN_VRM_TEMP_CRIT 75.0f                // VRM temp C, red
#endif

// ---- PC hardware shown on the board ----
#ifndef PC_GPU1_LABEL
#define PC_GPU1_LABEL "RX 6600"                   // GPU card 1 (stat keys gpu1_*: stats.ini maps them to a sensor)
#endif
#ifndef PC_GPU2_LABEL
#define PC_GPU2_LABEL "RTX 3050"                  // GPU card 2 (gpu2_*)
#endif
#ifndef PC_DRIVE_LABEL
#define PC_DRIVE_LABEL "C:"                       // drive card (c_used / c_total)
#endif
#ifndef PC_DRIVE_WARN_PCT
#define PC_DRIVE_WARN_PCT 85.0                    // drive used %, yellow
#endif
#ifndef PC_DRIVE_BAD_PCT
#define PC_DRIVE_BAD_PCT 95.0                     // drive used %, red
#endif
#ifndef LOCAL_AI_MODEL_NAME
#define LOCAL_AI_MODEL_NAME "Occamy-1.0"          // local model name on the Local AI face
#endif
#ifndef LOCAL_AI_MODEL_FULL
#define LOCAL_AI_MODEL_FULL "Occamy-1.0 35B-A3B"  // the same, full, on the working face
#endif

// ---- PC case RGB and fans (PcHwCtl names, Discord RGB / Fan tabs) ----
#ifndef HW_RGB_GROUPS
#define HW_RGB_GROUPS {"all", "All"}, {"main", "Motherboard"}, {"ram", "RAM"}  // PcHwCtl RGB groups: {name, label}, ...
#endif
#ifndef HW_FAN_TARGETS
#define HW_FAN_TARGETS {"all", "All fans"}, {"case", "Case fans"}, {"cpu-fans", "CPU fans"}, {"rear-fan", "Rear fan"}, {"front-fans", "Front fans"}, {"gpu-amd", "RX 6600 fan"}, {"gpu-nvidia", "RTX 3050 fan"}  // fan targets {PcHwCtl key, label}; "all" and "case" are PcHwCtl's / the board's groups, gpu-* get a Zero RPM menu
#endif
#ifndef HW_FAN_CASE_KEYS
#define HW_FAN_CASE_KEYS "cpu-fans", "rear-fan", "front-fans"  // what "case" means: one command per key
#endif
#ifndef HW_FAN_SHOWN
#define HW_FAN_SHOWN {"cpu_fans", "CPU fans"}, {"rear_fan", "Rear fan"}, {"front_fans", "Front fans"}, {"water_pump", "Water pump"}, {"gpu_amd", "RX 6600"}, {"gpu_nvidia", "RTX 3050"}  // Fan tab embed rows {state.txt key (_ for -), label}
#endif
#ifndef HW_FAN_HOLD_MIN
#define HW_FAN_HOLD_MIN 5, 15, 60                 // hold times a manual fan speed cycles through, min
#endif

// ---- Local AI (LocalAIHost) ----
#ifndef LOCAL_AI_IDLE_MIN
#define LOCAL_AI_IDLE_MIN 20                      // auto-unload minutes the Local Agent tab restores after "off"
#endif

// ---- Quota faces (Claude, Antigravity) ----
#ifndef QUOTA_SESSION_WARN_PCT
#define QUOTA_SESSION_WARN_PCT 75.0               // 5 h window used %, yellow
#endif
#ifndef QUOTA_SESSION_BAD_PCT
#define QUOTA_SESSION_BAD_PCT 90.0                // 5 h window used %, red
#endif
#ifndef QUOTA_WEEKLY_WARN_PCT
#define QUOTA_WEEKLY_WARN_PCT 80.0                // weekly used %, yellow
#endif
#ifndef QUOTA_WEEKLY_BAD_PCT
#define QUOTA_WEEKLY_BAD_PCT 95.0                 // weekly used %, red
#endif

// ---- Screens and frame rate ----
#ifndef SCREEN_MARQUEE_PX_S
#define SCREEN_MARQUEE_PX_S 30.0f                 // media title scroll speed, px/s
#endif
#ifndef SCREEN_MARQUEE_PAUSE_S
#define SCREEN_MARQUEE_PAUSE_S 1.5f               // pause at each end of the title
#endif
#ifndef VIEW_BUS_BUDGET_PCT
#define VIEW_BUS_BUDGET_PCT 85                    // share of the SPI bus the panels may use (`fps budget` changes it)
#endif
#ifndef VIEW_MIN_FRAME_MS
#define VIEW_MIN_FRAME_MS 50                      // canvas faces: at most 20 fps
#endif
#ifndef VIEW_GIF_MIN_FRAME_MS
#define VIEW_GIF_MIN_FRAME_MS 16                  // GIF face: at most ~60 fps (GIF frame times still apply)
#endif
#ifndef VIEW_METRICS_PAGE_MS
#define VIEW_METRICS_PAGE_MS 5000                 // metrics face pages through overflow cards this often
#endif

// ---- PC shutdown / restart announcements (pc_going.cpp) ----
#ifndef PCGOING_PREOFF_MS
#define PCGOING_PREOFF_MS (120 * 1000)            // after "restart/shut down" is announced, wait this long for the PC to go off
#endif
#ifndef PCGOING_PREOFF_UPDATE_MS
#define PCGOING_PREOFF_UPDATE_MS (60 * 60 * 1000)  // same, for an update restart / shutdown
#endif
#ifndef PCGOING_BACK_MS
#define PCGOING_BACK_MS (10 * 60 * 1000)          // after it went off, screens wait this long for it to come back
#endif
#ifndef PCGOING_BACK_UPDATE_MS
#define PCGOING_BACK_UPDATE_MS (2 * 60 * 60 * 1000)  // same, for an update restart (several reboots)
#endif

// ---- Night mode (board side; night.cpp) ----
#ifndef NIGHT_START_HOUR
#define NIGHT_START_HOUR 22                       // local hour the night window opens
#endif
#ifndef NIGHT_END_HOUR
#define NIGHT_END_HOUR 6                          // local hour it closes
#endif
#ifndef NIGHT_IDLE_ENGAGE_S
#define NIGHT_IDLE_ENGAGE_S (5 * 60)              // s without keyboard/mouse input on the PC before it engages in the window
#endif
#ifndef NIGHT_PANEL_PCT
#define NIGHT_PANEL_PCT 10                        // board panel backlight % while engaged
#endif
#ifndef NIGHT_STATUS_LED_PCT
#define NIGHT_STATUS_LED_PCT 20                   // status LED cap % while engaged
#endif
#ifndef NIGHT_RGB_PCT
#define NIGHT_RGB_PCT 10                          // PC case RGB % while engaged (PcHwCtl)
#endif
#ifndef NIGHT_GUARD_MS
#define NIGHT_GUARD_MS 10000                      // input during the night only ends it if this window shows a real return:
#endif
#ifndef NIGHT_GUARD_PX
#define NIGHT_GUARD_PX 50                         //   mouse moved more than this many px,
#endif
#ifndef NIGHT_GUARD_PRESSES
#define NIGHT_GUARD_PRESSES 3                     //   or this many key / button presses
#endif
#ifndef NIGHT_LASSO_REPEAT_MS
#define NIGHT_LASSO_REPEAT_MS 30000               // !night state resent to the PC this often (Process Lasso profile)
#endif
#ifndef NIGHT_SLEEP_S
#define NIGHT_SLEEP_S (60 * 60)                   // s without real input on the PC (false wakes don't count) before night mode sleeps it
#endif
#ifndef NIGHT_LEAVE_KM
#define NIGHT_LEAVE_KM 1.0                        // the phone (OwnTracks) going beyond this from home hibernates the PC (woken first if night mode put it to sleep)
#endif
#ifndef NIGHT_LEAVE_IDLE_S
#define NIGHT_LEAVE_IDLE_S (5 * 60)               // ...but a PC that is on only if nobody used it for this long
#endif
#ifndef NIGHT_NET_HOLD_BPS
#define NIGHT_NET_HOLD_BPS 1000000                // sleep / hibernate wait while net_down or net_up averages more than this (bytes/s, ~1 min)
#endif
#ifndef NIGHT_REQ_IGNORE
#define NIGHT_REQ_IGNORE "claude.exe"             // power request holders that don't hold sleep (comma list; claude.exe always holds EXECUTION)
#endif

// ---- Weather (board) ----
#ifndef WEATHER_HOME_NAME
#define WEATHER_HOME_NAME "Home"                  // place name until WEATHER_HOME or `weather home` gives one
#endif
#ifndef WEATHER_RAIN_URGENT_MIN
#define WEATHER_RAIN_URGENT_MIN 20                // rain ETA at or under this: red banner / red Discord post
#endif
#ifndef WEATHER_RAIN_SURE_MIN
#define WEATHER_RAIN_SURE_MIN 30                  // rain one source alone sees: a warning at this ETA or closer (else a quiet watch)
#endif
#ifndef WEATHER_RAIN_SURE_MMH
#define WEATHER_RAIN_SURE_MMH 2.0f                // ...or at this mm/h and up, whatever the other source says
#endif
#ifndef OWM_RAIN_SOLID_MMH
#define OWM_RAIN_SOLID_MMH 0.8f                   // OpenWeather alone: needs this peak mm/h...
#endif
#ifndef OWM_RAIN_SOLID_MIN
#define OWM_RAIN_SOLID_MIN 3                      // ...or this many rain minutes in a row to be more than a watch
#endif
#ifndef WEATHER_SHOWER_MIN
#define WEATHER_SHOWER_MIN 20                     // a shower usually lasts this many minutes...
#endif
#ifndef WEATHER_SHOWER_MAX
#define WEATHER_SHOWER_MAX 60                     // ...to this many (shown when no source has an end)
#endif
#ifndef WEATHER_TSTORM_MIN
#define WEATHER_TSTORM_MIN 30                     // a thunderstorm's rain: this many minutes...
#endif
#ifndef WEATHER_TSTORM_MAX
#define WEATHER_TSTORM_MAX 90                     // ...to this many
#endif
#ifndef WEATHER_UPWIND_POLL_MS
#define WEATHER_UPWIND_POLL_MS (10 * 60 * 1000)   // upwind probe (8 Open-Meteo calls) this often while rain is coming or falling at home
#endif
#ifndef WEATHER_TYPHOON_NEAR_S
#define WEATHER_TYPHOON_NEAR_S (6 * 3600)         // a typhoon arriving within this shows on the screens and gets per-minute posts
#endif
#ifndef WEATHER_AQI_MODERATE
#define WEATHER_AQI_MODERATE 50                   // air quality index above this: Moderate
#endif
#ifndef WEATHER_AQI_HIGH
#define WEATHER_AQI_HIGH 100                      // above this: High
#endif
#ifndef WEATHER_AQI_DANGEROUS
#define WEATHER_AQI_DANGEROUS 200                 // above this: Dangerous
#endif
#ifndef WEATHER_TYPHOON_IMMINENT_S
#define WEATHER_TYPHOON_IMMINENT_S (20 * 60)      // under this: typhoon posts turn red
#endif
#ifndef WEATHER_STORM_GUST_KMH
#define WEATHER_STORM_GUST_KMH 62                 // forecast gust at or over this (km/h, Beaufort 8) makes the hour a storm
#endif
#ifndef WEATHER_HEAVY_SHOWER_MMH
#define WEATHER_HEAVY_SHOWER_MMH 7.6f             // rain at or over this mm/h is a heavy shower
#endif
#ifndef WEATHER_SUMMARY_FIRST_HOUR
#define WEATHER_SUMMARY_FIRST_HOUR 6              // two-hourly #weather summary from this hour...
#endif
#ifndef WEATHER_SUMMARY_LAST_HOUR
#define WEATHER_SUMMARY_LAST_HOUR 20              // ...to this one (even hours)
#endif
#ifndef WEATHER_MORNING_HOUR
#define WEATHER_MORNING_HOUR 6                    // the 3-day forecast post goes out at this hour
#endif
#ifndef WEATHER_FORECAST_POLL_MS
#define WEATHER_FORECAST_POLL_MS (5 * 60 * 1000)  // Open-Meteo forecast poll
#endif
#ifndef WEATHER_AQI_POLL_MS
#define WEATHER_AQI_POLL_MS (30 * 60 * 1000)      // air quality poll
#endif
#ifndef WEATHER_CYCLONE_POLL_MS
#define WEATHER_CYCLONE_POLL_MS (30 * 60 * 1000)  // GDACS cyclone list poll
#endif

// ---- Rain nowcast (Open-Meteo 15-min grid, nowcast.cpp) ----
#ifndef NOWCAST_RING1_KM
#define NOWCAST_RING1_KM 10.0f                    // inner sample ring radius
#endif
#ifndef NOWCAST_RING2_KM
#define NOWCAST_RING2_KM 25.0f                    // outer sample ring radius
#endif
#ifndef NOWCAST_RAIN_MMH
#define NOWCAST_RAIN_MMH 0.5f                     // over this mm/h counts as rain
#endif
#ifndef NOWCAST_RAIN_END_MMH
#define NOWCAST_RAIN_END_MMH 0.3f                 // rain ends under this (hysteresis)
#endif
#ifndef NOWCAST_ALERT_ETA_MIN
#define NOWCAST_ALERT_ETA_MIN 60                  // warn when rain arrives within this
#endif
#ifndef NOWCAST_POLL_RAIN_MS
#define NOWCAST_POLL_RAIN_MS (5 * 60 * 1000)      // poll while warned or raining
#endif
#ifndef NOWCAST_POLL_NEAR_MS
#define NOWCAST_POLL_NEAR_MS (10 * 60 * 1000)     // poll while rain is in the grid
#endif
#ifndef NOWCAST_POLL_DRY_MS
#define NOWCAST_POLL_DRY_MS (15 * 60 * 1000)      // poll while the grid is dry
#endif

// ---- OpenWeather One Call 4.0 (owm.cpp; key: OWM_API_KEY) ----
#ifndef OWM_CALLS_PER_DAY
#define OWM_CALLS_PER_DAY 960                     // daily call budget (set the account's own cap to 1000)
#endif
#ifndef OWM_RAIN_MMH
#define OWM_RAIN_MMH 0.5f                         // minutely precipitation over this counts as rain
#endif
#ifndef OWM_POLL_RAIN_MS
#define OWM_POLL_RAIN_MS (90 * 1000)              // poll while warned, raining or under an alert
#endif
#ifndef OWM_POLL_MS
#define OWM_POLL_MS (120 * 1000)                  // poll otherwise
#endif

// ---- Tropical cyclones (GDACS, cyclone.cpp) ----
#ifndef CYCLONE_HOME_KM
#define CYCLONE_HOME_KM 300.0f                    // centre within this distance = "at home"
#endif
#ifndef CYCLONE_FETCH_KM
#define CYCLONE_FETCH_KM 3000.0f                  // storms further away are not fetched
#endif

// ---- Phone location (OwnTracks over MQTT; broker in MQTT_URI) ----
#ifndef OWNTRACKS_MAX_AGE_S
#define OWNTRACKS_MAX_AGE_S (2 * 3600)            // a fix older than this is ignored
#endif
#ifndef OWNTRACKS_HOME_KM
#define OWNTRACKS_HOME_KM 1.0                     // phone within this of home = at home (one weather engine)
#endif

// ---- PC helper (ESP32-Helper.exe; build.cmd turns HOST_* into C#, the sensor map stays in stats.ini) ----
#ifndef HOST_LASSO_INI
#define HOST_LASSO_INI "C:\\ProgramData\\ProcessLasso\\config\\prolasso.ini"  // Process Lasso config the night profile rewrites (original kept as .esp32-night)
#endif
#ifndef HOST_NIGHT_CORES
#define HOST_NIGHT_CORES "14-17"                  // cores night mode pins processes to, in efficiency mode (Core Ultra 7 265K E-cores are 2-5, 10-17)
#endif
#ifndef HOST_NIGHT_KEEP
#define HOST_NIGHT_KEEP "system,idle,registry,secure system,memory compression,smss,csrss,wininit,winlogon,services,lsass,lsaiso,fontdrvhost,dwm,audiodg,explorer,sihost,ctfmon,textinputhost,searchhost,startmenuexperiencehost,shellexperiencehost,logonui,lockapp,esp32-helper,pchwctl,localaihost,llama-server,processgovernor,processlasso,bitsumsessionagent,msmpeng,mpdefendercoreservice"  // processes night mode never pins (comma list)
#endif
#ifndef HOST_NIGHT_NO_WORD_MIN
#define HOST_NIGHT_NO_WORD_MIN 5                  // no !night from the board this long: the Process Lasso config goes back
#endif
#ifndef HOST_NIGHT_HEAVY_CORES
#define HOST_NIGHT_HEAVY_CORES 0.8                // an exe averaging this many cores (1.0 = one core, summed over instances) ...
#endif
#ifndef HOST_NIGHT_HEAVY_S
#define HOST_NIGHT_HEAVY_S 120                    //   ... for this long is heavy: night mode leaves it unpinned, and the board won't sleep the PC
#endif
#ifndef HOST_NIGHT_CALM_CORES
#define HOST_NIGHT_CALM_CORES 0.3                 // a heavy exe averaging less than this ...
#endif
#ifndef HOST_NIGHT_CALM_S
#define HOST_NIGHT_CALM_S 300                     //   ... for this long is calm again (pinned again at night)
#endif
#ifndef HOST_NIGHT_ALWAYS
#define HOST_NIGHT_ALWAYS ""                      // exes night mode keeps pinned even while heavy (comma list, e.g. searchindexer,tiworker); they still hold sleep
#endif
#ifndef HOST_HWCTL_PIPE
#define HOST_HWCTL_PIPE "PcHwCtl"                 // named pipe of the RGB / fan service (!hw)
#endif
#ifndef HOST_HWCTL_TIMEOUT_MS
#define HOST_HWCTL_TIMEOUT_MS 2000                // its reply timeout
#endif
#ifndef HOST_LOCALAI_PIPE
#define HOST_LOCALAI_PIPE "LocalAIHost"           // named pipe of the local model service (!ai)
#endif
#ifndef HOST_LOCALAI_TIMEOUT_MS
#define HOST_LOCALAI_TIMEOUT_MS 20000             // its reply timeout (a forced unload takes ~8 s)
#endif
#ifndef HOST_NIC_SKIP
#define HOST_NIC_SKIP "hyper-v,virtualbox,vmware,tap-,loopback,bluetooth,wan miniport"  // adapters whose description contains one of these are not the PC's MAC for Wake-on-LAN
#endif
#ifndef HOST_LHM_CPU
#define HOST_LHM_CPU true                         // LibreHardwareMonitor hardware groups to open
#endif
#ifndef HOST_LHM_GPU
#define HOST_LHM_GPU true
#endif
#ifndef HOST_LHM_MEMORY
#define HOST_LHM_MEMORY true
#endif
#ifndef HOST_LHM_MOTHERBOARD
#define HOST_LHM_MOTHERBOARD true
#endif
#ifndef HOST_LHM_STORAGE
#define HOST_LHM_STORAGE true
#endif
#ifndef HOST_LHM_NETWORK
#define HOST_LHM_NETWORK true
#endif
#ifndef HOST_LHM_CONTROLLER
#define HOST_LHM_CONTROLLER true
#endif
#ifndef HOST_LHM_PSU
#define HOST_LHM_PSU true
#endif
#ifndef HOST_LHM_BATTERY
#define HOST_LHM_BATTERY true
#endif
#ifndef HOST_FONT_REGULAR
#define HOST_FONT_REGULAR "ComicShannsMonoNerdFontPropo-Regular.otf"  // font file for text the board can't draw (Windows or per-user Fonts folder)
#endif
#ifndef HOST_FONT_BOLD
#define HOST_FONT_BOLD "ComicShannsMonoNerdFontPropo-Bold.otf"
#endif
#ifndef HOST_FONT_FALLBACK
#define HOST_FONT_FALLBACK "Segoe UI"             // family when those files are missing
#endif
#ifndef HOST_SHOT_DELETE_MIN
#define HOST_SHOT_DELETE_MIN 5.0                  // screenshot posts are deleted after this
#endif
#ifndef HOST_SHOT_BORDER_S
#define HOST_SHOT_BORDER_S 5                      // red "Screenshot Captured" border shows this long
#endif
#ifndef HOST_MON_POLL_MS
#define HOST_MON_POLL_MS 5000                     // monitor DDC/CI state feed (mon.*) period
#endif
#ifndef HOST_MON_DDC_POWER_MODELS
#define HOST_MON_DDC_POWER_MODELS "DELL E1715S"   // monitor models (DDC name, comma list) that also get DDC/CI power on/off; the rest only get Windows' display off
#endif
#ifndef HOST_MON_DDC_POWER_OFF
#define HOST_MON_DDC_POWER_OFF 4                  // VCP 0xD6 value for off on those (4 = DPM off: still answers DDC, wakes on input)
#endif
