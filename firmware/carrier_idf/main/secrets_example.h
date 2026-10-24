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
#define DISCORD_CHANNEL_ID ""   // optional: where it posts online / PC on / asleep

// Password for over-the-air firmware updates (arduino-cli / Arduino IDE
// network port). Empty = OTA disabled.
#define OTA_PASS ""
