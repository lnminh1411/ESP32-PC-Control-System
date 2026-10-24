// Discord bot: remote control from anywhere, with nothing opened on the
// router. The ESP32 holds an outgoing TLS WebSocket to Discord's gateway
// (certificate checked against the core's built-in CA bundle) and receives
// slash commands and button presses as gateway events; replies go back over
// HTTPS. Runs in its own task on core 0, so TLS never stalls the displays.
//
// Slash command (registered on DISCORD_GUILD_ID at boot, instant):
//   /actions -- opens an ephemeral control panel with status embed and buttons:
//               Door 1-4, Wake PC, Sleep/Restart/Shut down (confirm dialogs),
//               Refresh, PC reset (confirm dialog), Close.
// The panel is temporary and auto-deletes after ACTIONS_PANEL_TTL_MS of inactivity.
// Everything ends up as a line of the console command set (commands.h), run
// on the main loop, so the opto safety rules apply here too. Only
// DISCORD_OWNER_ID may use it; anyone else gets a refusal.
//
// Notifications to DISCORD_CHANNEL_ID (optional):
//   - Board online embed (green) with IP
//   - PC state changes (on = green, asleep/off = orange, unknown = grey)
//   - Action notifications via discordNotify() (PC reset = red, Door = blue)
//
// Setup (secrets.h): create an application + bot at
// discord.com/developers, invite it to your server with the "bot" and
// "applications.commands" scopes, then fill DISCORD_TOKEN, DISCORD_APP_ID,
// DISCORD_GUILD_ID, DISCORD_OWNER_ID (your user id) and optionally
// DISCORD_CHANNEL_ID. No privileged intents are needed.
#pragma once

#define ACTIONS_PANEL_TTL_MS 120000

void discordInit();                  // after netInit(): starts the bot task (no-op without a token)
void discordPoll();                  // call from loop(): runs commands the bot hands over
bool discordUp();                    // gateway session ready
void discordNotify(const char *what); // post action notification from main loop (non-blocking)
