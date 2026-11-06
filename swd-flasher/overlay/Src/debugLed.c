#include "../Inc/defines.h"
#include "../Inc/remoteDebugPort.h"
#include "../Inc/debugLed.h"

void DebugLedApply(uint8_t m)
{
#ifdef LED_GREEN
	digitalWrite(LED_GREEN, (m & LED_BIT_GREEN) ? SET : RESET);
#endif
#ifdef LED_ORANGE
	digitalWrite(LED_ORANGE, (m & LED_BIT_ORANGE) ? SET : RESET);
#endif
#ifdef LED_RED
	digitalWrite(LED_RED, (m & LED_BIT_RED) ? SET : RESET);
#endif
#ifdef UPPER_LED
	digitalWrite(UPPER_LED, (m & LED_BIT_UPPER) ? SET : RESET);
#endif
#ifdef LOWER_LED
	digitalWrite(LOWER_LED, (m & LED_BIT_LOWER) ? SET : RESET);
#endif
}
