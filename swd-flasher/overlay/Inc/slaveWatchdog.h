// Slave frame watchdog and LED driver. The upstream timeout (TIMER13 via ResetTimeout) is never
// started on the GD32E230 (TimeoutTimer_init is skipped for TARGET 3), so without this a slave would
// keep driving its last PWM after the master link dies. Uses msTicks (SysTick, 1 ms).
#ifndef SLAVE_WATCHDOG_H
#define SLAVE_WATCHDOG_H

#define SLAVE_FRAME_TIMEOUT_MS 800

void SlaveFrameSeen(void);       // called by the master-frame handler for every valid frame
void SlaveWatchdogUpdate(void);  // called in the slave main loop before SetBldcInput(pwmSlave)

#endif
