#pragma once
#include <stdbool.h>
#include <stdint.h>

void owntracksInit();
void owntracksStart();
bool owntracksEnabled();
bool owntracksValid();
bool owntracksGetLocation(double *lat, double *lon, uint32_t *tst, uint32_t *acc);
uint32_t owntracksAgeSec();
double owntracksDistanceTo(double homeLat, double homeLon);
bool owntracksIsMerged(double homeLat, double homeLon);
void owntracksAskIfStale();   // weather loop: reportLocation to a phone that has not reported for a while
