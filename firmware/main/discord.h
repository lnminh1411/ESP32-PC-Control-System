// Discord bot: remote control from anywhere, with nothing opened on the
// router. The ESP32 holds an outgoing TLS WebSocket to Discord's gateway
// (certificate checked against the core's built-in CA bundle) and receives
// slash commands and button presses as gateway events; replies go back over
// HTTPS. Runs in its own task on core 0, so TLS never stalls the displays.
//
// Slash command (registered on DISCORD_GUILD_ID at boot, instant):
//   /actions -- opens an ephemeral control panel with status embed and buttons:
//               Door 1-4, Wake PC, Sleep/Hibernate/Restart/Shut down (confirm dialogs),
//               Refresh, PC reset (confirm dialog), Close.
// The panel is temporary and auto-deletes after ACTIONS_PANEL_TTL_MS of inactivity.
// Everything ends up as a line of the console command set (commands.h), run
// on the main loop, so the opto safety rules apply here too. Only
// DISCORD_OWNER_ID may use it; anyone else gets a refusal.
//
// Channels (secrets.h, each optional):
//   DISCORD_CH_PC       board online, PC state changes, restart/update/power,
//                       door and reset notifications (discordNotify)
//   DISCORD_CH_WEATHER  rain alerts (home and phone) and a morning forecast
//   DISCORD_CH_LOG      the board's log, batched into code blocks
//   DISCORD_CH_CONTROL  one standing control panel message (the /actions
//                       buttons), edited in place and kept up to date
// PC and weather fall back to DISCORD_CHANNEL_ID when their own is empty.
//
// Setup (secrets.h): create an application + bot at
// discord.com/developers, invite it to your server with the "bot" and
// "applications.commands" scopes, then fill DISCORD_TOKEN, DISCORD_APP_ID,
// DISCORD_GUILD_ID, DISCORD_OWNER_ID (your user id) and optionally
// DISCORD_CHANNEL_ID. No privileged intents are needed.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ACTIONS_PANEL_TTL_MS 120000

void discordInit();                  // after netInit(): starts the bot task (no-op without a token)
void discordPoll();                  // call from loop(): runs commands the bot hands over
bool discordUp();                    // gateway session ready
void discordNotify(const char *what); // post action notification from main loop (non-blocking)

enum DiscordChannel { DC_PC, DC_WEATHER };
// Queue an embed for a channel (non-blocking, any task)
void discordPost(DiscordChannel ch, const char *title, const char *desc, uint32_t color);
// log.cpp hands every log line here for DISCORD_CH_LOG
void discordLog(const char *line, size_t len);
// While a Discord handler waits on the PC (hwctl/aictl), keeps the bot
// answering: heartbeats and new button clicks. No-op outside the bot task.
void discordWaitTick();
