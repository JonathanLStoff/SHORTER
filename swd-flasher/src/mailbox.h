// RAM mailbox of the MASTER controller (firmware: overlay/Inc/remoteDebugPort.h, described in API.md).
// The ESP32 reads telemetry and writes commands over the SWD debug port. Same address and offsets
// as the firmware; scripts/build_hoverboard.py checks MBX_ADDR, MBX_MAGIC and MBX_BYTES against it.
#pragma once
#include <stdint.h>
#include "swd.h"

#define MBX_ADDR   0x20001800u
#define MBX_MAGIC  0x3258424Du  // 'M' 'B' 'X' '2'
#define MBX_BYTES  0x54         // must equal MAILBOX_SIZE in the firmware
#define MBX_WORDS  (MBX_BYTES / 4)

#define MBX_OFF_HEARTBEAT   0x04
#define MBX_OFF_CMD_MASTER  0x08
#define MBX_OFF_CMD_SLAVE   0x0C
#define MBX_OFF_ENABLE      0x10
#define MBX_OFF_LED_MASTER  0x14
#define MBX_OFF_LED_SLAVE   0x18
#define MBX_OFF_POWER       0x1C

typedef struct {
    uint32_t magic;         // 0x00
    uint32_t heartbeat;     // 0x04  ESP32 counter, written last
    int32_t  cmdMaster;     // 0x08  -1000..1000, positive = forward
    int32_t  cmdSlave;      // 0x0C  -1000..1000, positive = forward
    uint32_t enable;        // 0x10  1 = motion allowed
    uint32_t ledMaster;     // 0x14  MBX_LED_* bits
    uint32_t ledSlave;      // 0x18  MBX_LED_* bits
    uint32_t power;         // 0x1C  MBX_POWER_OFF_KEY = switch both boards off
    uint32_t echo;          // 0x20  firmware copy of heartbeat
    uint32_t fwState;       // 0x24  MBX_FW_* bits
    float    vBus;          // 0x28  volts
    float    currentMaster; // 0x2C  amps
    float    currentSlave;  // 0x30  amps
    float    speedMaster;   // 0x34
    float    speedSlave;    // 0x38
    int32_t  odomMaster;    // 0x3C  hall steps
    int32_t  odomSlave;     // 0x40  hall steps
    uint32_t hallMaster;    // 0x44  0..7
    uint32_t hallSlave;     // 0x48  0..7, MBX_HALL_UNKNOWN before the first slave reply
    uint32_t slaveRx;       // 0x4C  slave replies received
    uint32_t uptimeMs;      // 0x50
} mbx_t;

_Static_assert(sizeof(mbx_t) == MBX_BYTES, "mbx_t must match the firmware mailbox");

// LED bits (same order as the firmware wState byte)
#define MBX_LED_GREEN  (1u << 0)
#define MBX_LED_ORANGE (1u << 1)
#define MBX_LED_RED    (1u << 2)
#define MBX_LED_UPPER  (1u << 3)
#define MBX_LED_LOWER  (1u << 4)
#define MBX_LED_ALL    0x1Fu

#define MBX_FW_DRIVING    (1u << 0)
#define MBX_FW_STOPPED    (1u << 1)
#define MBX_FW_HB_SEEN    (1u << 2)
#define MBX_FW_SLAVE_LINK (1u << 3)

#define MBX_POWER_OFF_KEY 0x2146464Fu   // 'O' 'F' 'F' '!'
#define MBX_HALL_UNKNOWN  0xFFu
#define MBX_TIMEOUT_MS    600           // firmware stops both wheels after this long without a new heartbeat

// Reads the whole mailbox (MBX_WORDS words). Returns SWD_OK or an swd error.
int mbx_read(swd_t *s, mbx_t *m);

// One command frame: both wheel commands and enable first, then the heartbeat (so the firmware sees
// the command only once it is complete). Returns SWD_OK or an swd error.
int mbx_write_command(swd_t *s, uint32_t heartbeat, int32_t cmd_master, int32_t cmd_slave, uint32_t enable);

// LEDs of both boards (MBX_LED_* masks). Applied without a heartbeat.
int mbx_write_led(swd_t *s, uint32_t master_mask, uint32_t slave_mask);

// Switches both boards off (the master tells the slave, then releases its power latch).
int mbx_power_off(swd_t *s);
