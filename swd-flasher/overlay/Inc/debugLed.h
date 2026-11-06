// Status LEDs belong to the debug port on both boards (see API.md).
//  master: the LEDs follow mailbox ledMaster (remoteDebugPort.c)
//  slave:  the LEDs follow the LED bits of the last master frame (slaveWatchdog.c)
// Every upstream LED write is rewritten to LedWrite() by scripts/build_hoverboard.py and does
// nothing, so battery display, test patterns and boot blinks never touch the LEDs.
#ifndef DEBUG_LED_H
#define DEBUG_LED_H

#include <stdint.h>

// Drive the LED pins from a 5-bit mask: LED_BIT_* in remoteDebugPort.h (bit order of wState)
void DebugLedApply(uint8_t mask);

#define LedWrite(pin, set) do { (void)(pin); (void)(set); } while (0)

#endif // DEBUG_LED_H
