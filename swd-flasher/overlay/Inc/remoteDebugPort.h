// Remote for the MASTER board: commands arrive in a RAM mailbox that the ESP32 writes over the
// SWD debug port. Part of swd-flasher/overlay; copied into the build tree by scripts/build_hoverboard.py.
// Full description: swd-flasher/API.md.
//
// The ESP32 must use the SAME address and field offsets as this struct (swd-flasher/src/mailbox.h);
// the build script checks MAILBOX_ADDR, MAILBOX_MAGIC and MAILBOX_SIZE against it.
// Every field is a 32-bit word so the SWD accesses are plain word reads/writes.
//
// The master is the only board with a mailbox. It drives its own wheel and forwards the slave wheel
// command and the slave LEDs to the slave over the master-slave cable (USART1).
#ifndef REMOTE_DEBUGPORT_H
#define REMOTE_DEBUGPORT_H

#include <stdint.h>

// 0x20001800: after .bss and below the top 1 KB kept for the stack (_estack = 0x20002000).
// scripts/build_hoverboard.py checks both limits.
#define MAILBOX_ADDR   0x20001800UL
#define MAILBOX_MAGIC  0x3258424DUL   // bytes 'M' 'B' 'X' '2' (little endian)

typedef struct {
    // ---- written by the firmware at boot
    uint32_t magic;         // 0x00  MAILBOX_MAGIC once the firmware is running
    // ---- written by the ESP32 (command)
    uint32_t heartbeat;     // 0x04  ESP32 changes it for every command frame, writes it LAST
    int32_t  cmdMaster;     // 0x08  master wheel, -1000..1000, positive = forward
    int32_t  cmdSlave;      // 0x0C  slave wheel,  -1000..1000, positive = forward
    uint32_t enable;        // 0x10  1 = motion allowed, 0 = both wheels stop
    uint32_t ledMaster;     // 0x14  master LEDs, LED_BIT_* (applied at once, no heartbeat needed)
    uint32_t ledSlave;      // 0x18  slave LEDs,  LED_BIT_* (sent to the slave in every frame)
    uint32_t power;         // 0x1C  write POWER_OFF_KEY to switch both boards off
    // ---- written by the firmware (telemetry)
    uint32_t echo;          // 0x20  copy of the last heartbeat seen
    uint32_t fwState;       // 0x24  FW_* bits
    float    vBus;          // 0x28  supply voltage on the master, volts
    float    currentMaster; // 0x2C  master DC current, amps
    float    currentSlave;  // 0x30  slave DC current, amps (from the slave reply)
    float    speedMaster;   // 0x34  master wheel speed (upstream realSpeed)
    float    speedSlave;    // 0x38  slave wheel speed
    int32_t  odomMaster;    // 0x3C  master wheel position, hall steps (one step = 4 degrees)
    int32_t  odomSlave;     // 0x40  slave wheel position, hall steps
    uint32_t hallMaster;    // 0x44  master hall state 0..7 (1..6 valid)
    uint32_t hallSlave;     // 0x48  slave hall state 0..7, HALL_UNKNOWN until the slave answered
    uint32_t slaveRx;       // 0x4C  number of replies received from the slave
    uint32_t uptimeMs;      // 0x50  master milliseconds since boot
} DebugMailbox;

// LED bits in ledMaster / ledSlave. Same bit order as the upstream wState byte, so the slave bits
// travel unchanged in the master->slave frame. Only LEDs that exist on the layout are driven.
#define LED_BIT_GREEN   (1u << 0)   // LED_GREEN  (PB3)
#define LED_BIT_ORANGE  (1u << 1)   // LED_ORANGE (PA15)
#define LED_BIT_RED     (1u << 2)   // LED_RED    (PB4)
#define LED_BIT_UPPER   (1u << 3)   // UPPER_LED  (PF1)
#define LED_BIT_LOWER   (1u << 4)   // LOWER_LED  (PB5)
#define LED_BITS_ALL    0x1Fu

// Other upstream wState bits (main.c STATE_Disable / STATE_Shutoff, same values)
#define WSTATE_DISABLE  0x40u      // slave: motor disabled
#define WSTATE_SHUTOFF  0x80u      // slave: switch off

// fwState bits
#define FW_DRIVING      (1u << 0)   // fresh heartbeat and enable = 1: commands are applied
#define FW_STOPPED      (1u << 1)   // stale heartbeat or enable = 0: both wheels commanded to 0
#define FW_HB_SEEN      (1u << 2)   // at least one heartbeat since boot
#define FW_SLAVE_LINK   (1u << 3)   // a slave reply arrived within SLAVE_LINK_TIMEOUT_MS

#define POWER_OFF_KEY   0x2146464FUL   // bytes 'O' 'F' 'F' '!'
#define HALL_UNKNOWN    0xFFu

#define MB_TIMEOUT_MS          600   // heartbeat unchanged this long = stop both wheels
#define SLAVE_LINK_TIMEOUT_MS  800   // no slave reply this long = FW_SLAVE_LINK cleared

// Wheel direction. Positive commands mean "this wheel pushes the board forward". The upstream mix
// used SPEED_COEFFICIENT -1 for both wheels (and the master sends -pwmSlave to the slave), so these
// keep that convention. Flip one if the bench test shows that wheel running backwards.
#define DIR_MASTER  (-1)
#define DIR_SLAVE   (-1)

#define MAILBOX_OFF_MAGIC       0x00
#define MAILBOX_OFF_HEARTBEAT   0x04
#define MAILBOX_OFF_CMD_MASTER  0x08
#define MAILBOX_OFF_CMD_SLAVE   0x0C
#define MAILBOX_OFF_ENABLE      0x10
#define MAILBOX_OFF_LED_MASTER  0x14
#define MAILBOX_OFF_LED_SLAVE   0x18
#define MAILBOX_OFF_POWER       0x1C
#define MAILBOX_OFF_ECHO        0x20
#define MAILBOX_OFF_FWSTATE     0x24
#define MAILBOX_OFF_VBUS        0x28
#define MAILBOX_OFF_CUR_MASTER  0x2C
#define MAILBOX_OFF_CUR_SLAVE   0x30
#define MAILBOX_OFF_SPD_MASTER  0x34
#define MAILBOX_OFF_SPD_SLAVE   0x38
#define MAILBOX_OFF_ODO_MASTER  0x3C
#define MAILBOX_OFF_ODO_SLAVE   0x40
#define MAILBOX_OFF_HALL_MASTER 0x44
#define MAILBOX_OFF_HALL_SLAVE  0x48
#define MAILBOX_OFF_SLAVE_RX    0x4C
#define MAILBOX_OFF_UPTIME      0x50
#define MAILBOX_SIZE            0x54

// Used by the patched main.c in place of the upstream speed/steer mix (master only)
int16_t DebugPortPwmMaster(void);
int16_t DebugPortPwmSlave(void);
// Called by the patched master-side frame handler for every valid slave reply
void DebugPortSlaveReply(void);

#endif // REMOTE_DEBUGPORT_H
