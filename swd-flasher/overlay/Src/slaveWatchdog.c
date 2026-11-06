// SLAVE: the only command source is the master frame over USART1 (commsMasterSlave.c).
//  - motor: pwmSlave from the frame; 0 when no valid frame arrived for SLAVE_FRAME_TIMEOUT_MS
//    (or none since boot). The frame's disable bit is handled by upstream main.c (enable = RESET).
//  - LEDs: the LED bits (wState bits 0..4) of the last frame. Off until the first frame.
#include "../Inc/defines.h"
#include "../Inc/slaveWatchdog.h"
#include "../Inc/remoteDebugPort.h"
#include "../Inc/debugLed.h"

#ifdef SLAVE

extern uint32_t msTicks;
extern int16_t pwmSlave;
extern uint8_t wState;

static volatile uint32_t lastFrameMs = 0;
static volatile uint8_t  frameSeen = 0;

void SlaveFrameSeen(void)
{
	lastFrameMs = msTicks;
	frameSeen = 1;
}

void SlaveWatchdogUpdate(void)
{
	DebugLedApply(frameSeen ? (uint8_t)(wState & LED_BITS_ALL) : 0);

	if (!frameSeen || (uint32_t)(msTicks - lastFrameMs) > SLAVE_FRAME_TIMEOUT_MS)
		pwmSlave = 0;
}

#else

void SlaveFrameSeen(void) {}
void SlaveWatchdogUpdate(void) {}

#endif
