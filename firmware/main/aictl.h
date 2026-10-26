#pragma once

// Local AI stack control: commands for the LocalAIHost service
// (E:\Local LLM\Service), relayed by the PC service. The board sends `!ai <id> <command>`
// over USB HID; the answer comes back as the text stat
// aictl.reply = "<id> ok[ <info>]" | "<id> err <why>". Same shape as hwctl.h.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sends one command (e.g. "model load"). Returns an error string, or nullptr
// with *id set to the request id. Callable from any task.
const char *aiSend(const char *command, uint32_t *id);

// Waits for the answer to request id: true with "ok ..." or "err ..." in out,
// false on timeout. Blocks, so never call it from the main loop.
bool aiWaitReply(uint32_t id, uint32_t timeoutMs, char *out, size_t outSize);

// Send + wait; true when the answer starts with "ok".
bool aiRun(const char *command, uint32_t timeoutMs, char *out, size_t outSize);

void aiPoll();                          // main loop: logs new replies
void aiConsole(const char *command);    // console `ai <command>`

#ifdef __cplusplus
}
#endif
