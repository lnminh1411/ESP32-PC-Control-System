// Text commands, one per line, from any transport (console.cpp today).
// Replies go out through logMsg(), so they reach both serial ports.
//   help | status | view <1|2|3> <gif|status|stats> | door <1-4> | reset
//   bl <1|2|3|all> <0-100|on|off> | led <auto|on|off|sweep|0-100> | pcstats [text]
// Home Assistant sends the same lines over MQTT (ha.h).
#pragma once

void commandRun(char *line);   // tokenises line in place
