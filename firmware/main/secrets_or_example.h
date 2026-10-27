// secrets.h when it exists (local builds), else the placeholders (CI).
#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

// Keys added after a local secrets.h was made: default to off
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
