// secrets.h when it exists (local builds), else the placeholders (CI).
#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

// Keys added after a local secrets.h was made: default to off
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
#ifndef WEATHER_HOME
#define WEATHER_HOME ""
#endif
