// Network credentials. Copy this file to secrets.h (gitignored) and fill it
// in; without a secrets.h the firmware still builds (CI does), with these
// placeholders, and simply never joins a network.
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

// Password for over-the-air firmware updates (arduino-cli / Arduino IDE
// network port). Empty = OTA disabled.
#define OTA_PASS ""

// OwnTracks over MQTT (TLS HiveMQ Cloud or similar). Empty MQTT_URI = phone engine off.
#define MQTT_URI        ""
#define MQTT_USER       ""
#define MQTT_PASS       ""
#define OWNTRACKS_TOPIC "owntracks/#"

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
