#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void powerPoll();
const char *powerRequest(const char *action);
const char *powerWake();
const char *screenshotRequest();   // fire-and-forget: ask the PC service to post a screenshot to #logging

#ifdef __cplusplus
}
#endif
