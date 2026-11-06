// Control mode: the ESP32 drives the hoverboard through the MASTER's debug port (see mailbox.h, API.md).
// Only active with CONTROL_MODE 1 in config.h. Commands come from the serial console.
#pragma once
#include "swd.h"

// master = the first SWD connector. The slave is commanded through the master. Never returns.
void control_run(swd_t *master);

// CONTROL_MODE 2: raw RAM peek/poke console for bench tests (see control.c). Never returns.
void control_run_raw(swd_t *target);
