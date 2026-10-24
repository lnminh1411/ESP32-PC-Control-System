#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wolInit();
void wolPoll();
const char *wolSend();
bool wolSetMac(const char *s);
const char *wolMacStr();

#ifdef __cplusplus
}
#endif
