// The three desk screens (views pc / media / ai). Each view shows one "face"
// chosen from the PC state; views.cpp asks for the face, redraws the canvas
// when it is dirty or its period elapses, and handles GIF faces itself.
//   pc    : PC stats while on; GIF while off/asleep; GIF + warning box while
//           restarting/updating
//   media : now playing + uptime/network while on (rain warning on top);
//           weather while off/restarting/updating
//   ai    : Claude / Antigravity usage while on, local AI full screen while it
//           works; big clock with the PC state chip while off
#pragma once
#include <stdint.h>

#include "display.h"

enum Face : uint8_t {
  FACE_NONE,
  FACE_PC_STATS,
  FACE_GIF,           // plain GIF, no canvas
  FACE_GIF_RESTART,   // GIF + "PC is Restarting" box in the overlay band
  FACE_GIF_UPDATE,    // GIF + "PC is Updating" box
  FACE_MEDIA,
  FACE_WEATHER,
  FACE_AI_USAGE,
  FACE_LOCAL_AI,
  FACE_CLOCK,
};

constexpr int OVERLAY_Y0 = 248;   // GIF overlay band, multiples of 8 (display BAND)
constexpr int OVERLAY_Y1 = 304;
// Canvas colour inside the overlay band that means "GIF shows through here",
// so the warning box sits on top of the GIF instead of blanking the rows.
constexpr uint16_t C_GIF_KEY = 0xF81F;

Face screensFace(View v);                       // face to show now for an auto view
extern Face screensForce[N_PANELS];             // `face` command: pin a face (docs, screenshots)
extern bool screensNight;                       // night mode: the PC-off faces everywhere, pins ignored
bool screensIsGif(Face f);                      // FACE_GIF*
void screensDraw(Face f, LGFX_Sprite &c);       // full canvas redraw (GIF faces: the band only)
uint32_t screensPeriodMs(Face f);               // redraw period with no new data; 0 = data-driven
bool localAiBusy();                             // local model loading / working
