// Home Assistant over MQTT (HA's Mosquitto broker add-on + MQTT integration).
//
// On connect the board announces itself through MQTT discovery as one
// device, "ESP32 PC Control", with:
//   buttons  Door button 1-4, PC reset
//   lights   Screen 1-3 backlight (on/off + brightness 0-100)
//   selects  Screen 1-3 view (gif | status | stats), Status LED (auto | on | off)
//   sensors  PC state, last message; diagnostics: uptime, Wi-Fi signal, IP,
//            chip temperature, SPI bus load, GIF fps, free PSRAM
// Every control sends a plain text command to <base>/cmd (the same command
// set as the serial console, commands.h), so the safety rules in optos.cpp
// apply to Home Assistant too. State goes out as one JSON document on
// <base>/state (on change, and every 10 s); <base>/avail is online/offline
// (broker-held last will). Discovery is re-sent when HA restarts.
#pragma once

void haInit();     // after netInit()
void haPoll();     // call from loop(): runs queued commands, publishes state
bool haUp();       // connected to the broker
