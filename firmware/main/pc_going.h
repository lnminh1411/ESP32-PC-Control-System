#pragma once
#include <stdbool.h>
#include <stdint.h>

enum GoingKind : uint8_t {
  GO_NONE,
  GO_RESTART,
  GO_SHUTDOWN,
  GO_UPDATE_RESTART,
  GO_UPDATE_SHUTDOWN,
  GO_SLEEP
};

void pcGoingOnSample();          // call at every @e (pcstats.cpp, after sampleSeq++)
void pcGoingPoll();              // call from the main loop
GoingKind pcGoing();             // latched announcement, GO_NONE if none
const char *pcGoingLabel();      // "RESTARTING", "UPDATING", "SHUTTING DOWN", "GOING TO SLEEP", ""
const char *pcGoingBy();         // initiating process ("" if none)
const char *pcGoingWhy();        // reason text ("" if none)
uint32_t pcGoingAgeMs();         // since the announcement
uint32_t pcGoingLeftMs();        // until we stop expecting the PC back; 0 = n/a
bool pcGoingHoldsScreens();      // keep backlights on + show status while true
const char *pcGoingWakeBlock();  // nullptr = wake ok, else a short reason
void pcGoingInject(const char *kind);   // test hook for `going <kind|clear>`
const char *pcLastDown();        // latched pc.last_down text ("" if never seen)
const char *pcLastDownAt();      // latched pc.last_down_at text
