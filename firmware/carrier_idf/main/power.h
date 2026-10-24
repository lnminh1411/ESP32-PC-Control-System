#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void powerPoll();
const char *powerRequest(const char *action);
const char *powerWake();

#ifdef __cplusplus
}
#endif
