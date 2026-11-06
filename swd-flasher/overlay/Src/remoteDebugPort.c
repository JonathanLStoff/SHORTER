// Master-side remote that takes its orders from the RAM mailbox (see remoteDebugPort.h, API.md).
// The ESP32 is the only writer of the command fields. The firmware stops both wheels when the
// heartbeat stops changing for MB_TIMEOUT_MS, when enable is 0, and before the first heartbeat.
// The upstream speed/steer mix is bypassed: main.c takes the two wheel values from
// DebugPortPwmMaster() / DebugPortPwmSlave().
#include "../Inc/defines.h"
#include "../Inc/it.h"
#include "../Inc/remote.h"
#include "../Inc/debugLed.h"
#include "../Inc/bldc.h"
#include "../Inc/commsMasterSlave.h"

#ifdef REMOTE_DEBUGPORT

#define MB (*(volatile DebugMailbox *)MAILBOX_ADDR)

extern uint8_t bRemoteTimeout;          // any remote sets this to 1 to stop the motors (main.c)
extern int32_t speed;                   // main.c (ShutOff() ramps it; kept at 0 here)
extern int32_t steer;                   // main.c
extern uint32_t msTicks;                // it.c, 1 ms tick
extern float batteryVoltage;            // main.c
extern float currentDC;                 // bldc.c
extern float realSpeed;                 // bldc.c
extern int32_t iOdom;                   // bldc.c
extern volatile uint8_t hall;           // bldc.c, last hall state read by the commutation
extern uint8_t wStateSlave;             // main.c, sent to the slave in every frame
extern DataSlave oDataSlave;            // main.c, filled from the slave replies
extern int32_t ShutOff(void);           // main.c

static uint8_t  inited = 0;
static uint8_t  seenHeartbeat = 0;
static uint32_t lastHeartbeat = 0;
static uint32_t lastChangeMs = 0;
static int16_t  pwmM = 0, pwmS = 0;

static volatile uint32_t slaveRx = 0;
static volatile uint32_t lastSlaveRxMs = 0;

static int16_t clamp1000(int32_t v)
{
	if (v > 1000) return 1000;
	if (v < -1000) return -1000;
	return (int16_t)v;
}

int16_t DebugPortPwmMaster(void) { return pwmM; }
int16_t DebugPortPwmSlave(void)  { return pwmS; }

void DebugPortSlaveReply(void)
{
	slaveRx++;
	lastSlaveRxMs = msTicks;
}

static void PowerOff(void)
{
	// Tell the slave a few times (one lost frame must not leave it running), then switch off.
	// ShutOff() sends one more frame, releases SELF_HOLD and never returns.
	pwmM = pwmS = 0;
	speed = 0;
	steer = 0;
	SetEnable(RESET);
	SetBldcInput(0);
	wStateSlave = WSTATE_SHUTOFF | WSTATE_DISABLE;
	for (int i = 0; i < 5; i++)
	{
		SendSlave(0);
		Delay(10);
	}
	ShutOff();
}

void RemoteCallback(void) {}

void RemoteUpdate(void)
{
	if (!inited)	// first call after reset: take ownership of the mailbox, motors off, LEDs off
	{
		MB.heartbeat = 0;
		MB.cmdMaster = 0;
		MB.cmdSlave = 0;
		MB.enable = 0;
		MB.ledMaster = 0;
		MB.ledSlave = 0;
		MB.power = 0;
		MB.echo = 0;
		MB.hallSlave = HALL_UNKNOWN;
		MB.magic = MAILBOX_MAGIC;
		inited = 1;
	}

	if (MB.power == POWER_OFF_KEY)
		PowerOff();

	// Telemetry for the ESP32
	uint32_t now = msTicks;
	MB.uptimeMs = now;
	MB.vBus = batteryVoltage;
	MB.currentMaster = currentDC;
	MB.speedMaster = realSpeed;
	MB.odomMaster = iOdom;
	MB.hallMaster = hall & 7u;
	MB.currentSlave = oDataSlave.currentDC;
	MB.speedSlave = oDataSlave.realSpeed;
	MB.odomSlave = oDataSlave.iOdom;
	// the patched slave reply carries its hall state in wState bits 4..6, bit 7 marks it as valid
	MB.hallSlave = (oDataSlave.wState & 0x80u) ? ((oDataSlave.wState >> 4) & 7u) : HALL_UNKNOWN;
	MB.slaveRx = slaveRx;
	uint8_t slaveLink = slaveRx && ((uint32_t)(now - lastSlaveRxMs) < SLAVE_LINK_TIMEOUT_MS);

	// Heartbeat: the ESP32 writes it last, so a changed value means the command fields are complete
	uint32_t hb = MB.heartbeat;
	if (hb != lastHeartbeat)
	{
		lastHeartbeat = hb;
		lastChangeMs = now;
		seenHeartbeat = 1;
	}
	MB.echo = hb;

	uint8_t fresh = seenHeartbeat && ((uint32_t)(now - lastChangeMs) < MB_TIMEOUT_MS);
	uint32_t fw = (seenHeartbeat ? FW_HB_SEEN : 0) | (slaveLink ? FW_SLAVE_LINK : 0);

	if (fresh && MB.enable)
	{
		pwmM = DIR_MASTER * clamp1000(MB.cmdMaster);
		pwmS = DIR_SLAVE * clamp1000(MB.cmdSlave);
		bRemoteTimeout = 0;
		ResetTimeout();		// keep the pwm timeout from stopping the motors
		fw |= FW_DRIVING;
	}
	else
	{
		pwmM = pwmS = 0;
		bRemoteTimeout = 1;	// main.c: enable = RESET, and the slave gets STATE_Disable
		fw |= FW_STOPPED;
	}
	speed = 0;		// the upstream mix is bypassed; ShutOff() must not see a stale speed
	steer = 0;
	MB.fwState = fw;

	// LEDs: master pins now, slave bits go out in the next frame (main.c keeps the disable bit itself)
	DebugLedApply((uint8_t)(MB.ledMaster & LED_BITS_ALL));
	wStateSlave = (uint8_t)((wStateSlave & WSTATE_DISABLE) | (MB.ledSlave & LED_BITS_ALL));
}

#endif // REMOTE_DEBUGPORT
