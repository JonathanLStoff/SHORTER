#pragma once

typedef enum {
    LED_OFF,
    LED_SEARCH_BLUE, // blinking blue: looking for a motor controller
    LED_INSTALL_YELLOW, // solid yellow: erasing / programming / verifying
    LED_DONE_GREEN,  // solid green: complete
    LED_ERROR_RED,   // solid red: error
} led_mode_t;

void led_init(void);
void led_set(led_mode_t mode); // thread-safe; the LED task renders it
void led_selftest(void);       // red, green, blue, off (blocking, ~1 s)
