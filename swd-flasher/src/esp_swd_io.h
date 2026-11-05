// ESP32 GPIO backend for the bit-banged SWD master.
#pragma once
#include <stdint.h>

#include "swd.h"

typedef struct {
    int swclk, swdio, nrst; // GPIO numbers, nrst = -1 if unused
    uint32_t half_cycles;   // CPU cycles per half clock period
} esp_swd_pins_t;

extern const swd_io_t ESP_SWD_IO;

// Configure the GPIOs and the timing. Call once per target before swd_init().
void esp_swd_pins_init(esp_swd_pins_t *p, int swclk, int swdio, int nrst);

// Wiring check: read SWDIO with a weak pull-down, then a weak pull-up.
// Floating/unconnected: 0 then 1. Stuck 0/0 = shorted to ground (or driven low),
// stuck 1/1 = held high by the board (normal when connected to a powered target).
void esp_swd_line_check(const esp_swd_pins_t *p, int *with_pulldown, int *with_pullup);
