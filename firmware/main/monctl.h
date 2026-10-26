#pragma once

// PC Monitor Control: DDC/CI on the PC's monitors, done by the PC service's
// user-session half (host/pc_stats/Monitors.cs). The board sends
// `!mon <id> <command>` to the PC; the answer comes back as the text stat
// monctl.reply = "<id> ok" | "<id> err <why>". Same shape as aictl.h.
// Commands: bright <n|all> <0-100>, power <n|all> on|off (n counts from the
// left). The monitors themselves arrive as mon.count and mon.<n>.name / main /
// bright / power.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sends one command (e.g. "bright all 50"). Returns an error string, or
// nullptr with *id set to the request id. Callable from any task.
const char *monSend(const char *command, uint32_t *id);

// Send + wait for the answer; true when it starts with "ok". Blocks, so never
// call it from the main loop.
bool monRun(const char *command, uint32_t timeoutMs, char *out, size_t outSize);

void monPoll();                          // main loop: logs new replies
void monConsole(const char *command);    // console `mon <command>`

#ifdef __cplusplus
}
#endif
