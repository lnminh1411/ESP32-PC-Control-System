#pragma once

// PC hardware control: RGB commands for PcHwCtl (E:\PC Hardware Controller),
// relayed by the PC service. The board sends `!hw <id> <command>` over USB HID;
// the answer comes back as the text stat hw.reply = "<id> ok" | "<id> err <why>".

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sends one command (e.g. "rgb set main #ff0000"). Returns an error string, or
// nullptr with *id set to the request id. Callable from any task.
const char *hwSend(const char *command, uint32_t *id);

// Waits for the answer to request id: true with "ok" or "err ..." in out, false
// on timeout. Blocks, so never call it from the main loop (it reads PC lines).
bool hwWaitReply(uint32_t id, uint32_t timeoutMs, char *out, size_t outSize);

// Send + wait in one call; out gets "ok", "err ..." or the send error.
bool hwRun(const char *command, uint32_t timeoutMs, char *out, size_t outSize);

void hwPoll();                          // main loop: logs new replies
void hwConsole(const char *command);    // console `hw <command>`

#ifdef __cplusplus
}
#endif
