// GD32E230 (Cortex-M23) flash programming over SWD.
#pragma once
#include "swd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GD32_OK = 0,
    GD32_NO_TARGET,    // nothing answering on SWD (keep searching)
    GD32_WRONG_CORE,   // answers, but not a Cortex-M23 -> refuse to touch it
    GD32_UNLOCKED_RETRY, // chip was read-protected, protection removed; reconnect and call again
    GD32_ERROR,        // erase/program/verify failure
} gd32_result_t;

typedef struct {
    void (*installing)(void *user); // called once a valid target is found and writing starts
    void *user;
    const char *name; // for logging
} gd32_cb_t;

// Connect to the target, and if it is a GD32E230 program `image` and verify it.
// On GD32_OK the target has been reset and is running the new firmware.
gd32_result_t gd32_flash(swd_t *s, const uint8_t *image, size_t len, int allow_unlock,
                         const gd32_cb_t *cb);

#ifdef __cplusplus
}
#endif
