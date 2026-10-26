#pragma once
#include <stdint.h>

void consolePoll();               // read from UART and USB, run commands
void consoleUploadWindow(uint32_t ms);
