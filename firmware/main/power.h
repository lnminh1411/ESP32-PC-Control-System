#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void powerPoll();
const char *powerRequest(const char *action);
const char *powerWake();
const char *screenshotRequest();   // fire-and-forget: ask the PC service to post a screenshot to #logging
// Hibernation on the PC as last reported (pc.hib, kept over restarts): 1 on,
// 0 off, -1 never heard. Off: hibernate requests are refused on the board and
// the Discord panel hides its Hibernate button
int powerHibernateOn();

#ifdef __cplusplus
}
#endif
