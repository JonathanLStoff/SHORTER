# Hoverboard Debug-Port Control API

Version: mailbox v2 (`MBX2`). Firmware: RoboDurden Gen2.x-GD32 @ c63b29a + `swd-flasher/overlay`, layout 2.3.2 (GD32E230C8).
Status: built and checked in the emulator. **Not tested on hardware yet.**

## 1. System overview

```
 ESP32-C6 (swd-flasher, CONTROL_MODE 1)
    │  SWD (connector 1: SWCLK GPIO1, SWDIO GPIO0)
    │  reads/writes RAM mailbox @ 0x20001800, every 50 ms
    ▼
 MASTER board (firmware/master.bin)
    │  drives master wheel, master LEDs
    │  USART1 PA2/PA3, 115200 8N1, one frame every 10 ms
    ▼
 SLAVE board (firmware/slave.bin)
       drives slave wheel, slave LEDs; replies with its telemetry
```

* The **only** command input is the master's debug port. The slave obeys only master frames.
* No foot-pad, IMU, balance, button, battery or inactivity logic is in either image.
* Hall sensors are used **only** for commutation (which coil to power) and are reported back. They never start, stop, speed up or reverse a wheel.

## 2. Console (ESP32 serial, `CONTROL_MODE 1`)

Boots **disarmed**. Commands persist until changed (`CONTROL_CMD_HOLD_MS 0`).

| Command | Effect |
|---|---|
| `help` | List commands |
| `arm` | Allow motion (wheel commands stay 0 until `s`) |
| `disarm` | Both wheels 0, motion not allowed |
| `s <master> [slave]` | Wheel commands, -1000..1000, positive = forward. One value = both wheels. Example: `s 200 -200` spins in place |
| `stop` | Both wheels 0, stays armed |
| `led <master\|slave\|both> <green\|orange\|red\|upper\|lower\|all> on\|off` | Set LEDs. Example: `led slave red on` |
| `st` | Print status: state, heartbeat, Vbus, per wheel current / speed / odometry / hall / LEDs, slave link |
| `off` | Switch both boards off (button boots them again) |

Status output format (example values):
```
DRIVING slave-link-ok hb=812 echo=812 enable=1 cmd=200/-200 up=40512ms Vbus=24.10V
master: I=0.84A speed=1.2 odom=310 hall=4 leds=0x05
slave:  I=0.80A speed=-1.2 odom=-305 hall=2 leds=0x10 replies=3990
```
A hall value with `!` (0 or 7) means a sensor is missing or broken: that wheel's outputs are off.

## 3. Master mailbox (RAM, SWD)

Base `0x20001800`, size `0x54`, all fields 32-bit little endian.
Defined in `overlay/Inc/remoteDebugPort.h` (firmware) and `src/mailbox.h` (ESP32); the build fails if address, magic or size differ.

### 3.1 Command fields (ESP32 writes)

| Offset | Name | Type | Range / unit | Description |
|---|---|---|---|---|
| 0x04 | `heartbeat` | u32 | any | Change it for every command frame. **Write last.** |
| 0x08 | `cmdMaster` | i32 | -1000..1000 | Master wheel, positive = forward. Clamped |
| 0x0C | `cmdSlave` | i32 | -1000..1000 | Slave wheel, positive = forward. Clamped |
| 0x10 | `enable` | u32 | 0 / 1 | 1 = commands applied, 0 = both wheels 0 |
| 0x14 | `ledMaster` | u32 | LED bits | Master LEDs. Applied within 10 ms, no heartbeat needed |
| 0x18 | `ledSlave` | u32 | LED bits | Slave LEDs. Sent in every frame, no heartbeat needed |
| 0x1C | `power` | u32 | `0x2146464F` | `'OFF!'` switches both boards off. Other values ignored |

### 3.2 Telemetry fields (firmware writes, every 10 ms)

| Offset | Name | Type | Unit | Description |
|---|---|---|---|---|
| 0x00 | `magic` | u32 | | `0x3258424D` (`'MBX2'`) once the firmware runs |
| 0x20 | `echo` | u32 | | Last heartbeat seen |
| 0x24 | `fwState` | u32 | bits | See 3.4 |
| 0x28 | `vBus` | f32 | V | Supply voltage at the master (uncalibrated, low-pass filtered) |
| 0x2C | `currentMaster` | f32 | A | Master DC current (0.2 A per ADC count) |
| 0x30 | `currentSlave` | f32 | A | Slave DC current, from the slave reply |
| 0x34 | `speedMaster` | f32 | rev/s | Master wheel speed, signed |
| 0x38 | `speedSlave` | f32 | rev/s | Slave wheel speed, signed |
| 0x3C | `odomMaster` | i32 | hall steps | 1 step = 4° of wheel rotation |
| 0x40 | `odomSlave` | i32 | hall steps | |
| 0x44 | `hallMaster` | u32 | 0..7 | Raw hall state, 1..6 valid |
| 0x48 | `hallSlave` | u32 | 0..7 | `0xFF` until the first slave reply |
| 0x4C | `slaveRx` | u32 | count | Valid replies received from the slave |
| 0x50 | `uptimeMs` | u32 | ms | Master time since boot |

### 3.3 LED bits (`ledMaster`, `ledSlave`, frame `wState` bits 0..4)

| Bit | Mask | LED | Pin |
|---|---|---|---|
| 0 | 0x01 | green | PB3 |
| 1 | 0x02 | orange | PA15 |
| 2 | 0x04 | red | PB4 |
| 3 | 0x08 | upper | PF1 |
| 4 | 0x10 | lower | PB5 |

All LEDs are off at boot and stay as last commanded. The firmware never drives them itself.

### 3.4 `fwState` bits

| Bit | Name | Meaning |
|---|---|---|
| 0 | `FW_DRIVING` | Fresh heartbeat and `enable = 1`: commands applied |
| 1 | `FW_STOPPED` | Stale heartbeat or `enable = 0`: both wheels 0, slave disabled |
| 2 | `FW_HB_SEEN` | At least one heartbeat since boot |
| 3 | `FW_SLAVE_LINK` | A slave reply arrived within the last 800 ms |

### 3.5 Write protocol

1. Wait for `magic == 0x3258424D` (≈170 ms after power-up).
2. Every ≤ 50 ms: write `cmdMaster`, `cmdSlave`, `enable`, then `heartbeat` (new value).
3. Check `echo == heartbeat` to confirm the firmware consumed it.
4. LEDs: write `ledMaster` / `ledSlave` at any time.
5. Off: write `0x2146464F` to `power`.

C API (ESP32, `src/mailbox.h`):
```c
int mbx_read(swd_t *s, mbx_t *m);                                     // whole mailbox
int mbx_write_command(swd_t *s, uint32_t hb, int32_t cmd_master, int32_t cmd_slave, uint32_t enable);
int mbx_write_led(swd_t *s, uint32_t master_mask, uint32_t slave_mask);
int mbx_power_off(swd_t *s);
```
All return `SWD_OK` or an SWD error.

## 4. Master ↔ slave link (USART1, PA2 TX / PA3 RX, 115200 8N1)

CRC: CRC16-CCITT (poly 0x1021, init 0) over all bytes before the CRC field. Little endian.

### 4.1 Master → slave (6 bytes, every 10 ms)

| Byte | Field | Type | Description |
|---|---|---|---|
| 0 | start | u8 | `'/'` (0x2F) |
| 1-2 | `pwm` | i16 | Slave wheel, -1000..1000 (= `cmdSlave`) |
| 3 | `wState` | u8 | Bits 0..4 LEDs (3.3), bit 5 unused (0), bit 6 `0x40` disable, bit 7 `0x80` switch off |
| 4-5 | crc | u16 | |

### 4.2 Slave → master reply (16 bytes, after each valid frame)

| Byte | Field | Type | Description |
|---|---|---|---|
| 0 | start | u8 | `'/'` |
| 1 | `wState` | u8 | Bits 0..3 upstream (unused), bits 4..6 slave hall state, bit 7 = 1 (hall valid) |
| 2-5 | `currentDC` | f32 | A |
| 6-9 | `realSpeed` | f32 | rev/s |
| 10-13 | `iOdom` | i32 | hall steps |
| 14-15 | crc | u16 | |

## 5. Behaviour and timing

| Event | Result |
|---|---|
| Power-up (button) | Firmware holds its power latch (PB2), one 150 ms beep (master), LEDs off, wheels stopped |
| No heartbeat yet | Both wheels 0, slave disabled |
| Heartbeat unchanged **600 ms** | Both wheels 0, slave disabled (`FW_STOPPED`) |
| `enable = 0` | Both wheels 0, slave disabled |
| No valid master frame on the slave for **800 ms** | Slave wheel 0 (slave's own watchdog) |
| Bad CRC frame | Ignored |
| `power = 'OFF!'` | Master sends 5+ switch-off frames, slave and master release PB2 |
| Main loop period | 5 ms; commands, telemetry and frames every 10 ms |
| Power button after boot | Ignored |

Command → motor: master wheel input = `-cmdMaster`, slave wheel input = `cmdSlave` (`DIR_MASTER` / `DIR_SLAVE` = -1 in `remoteDebugPort.h`, matching upstream's mirrored wheels). The firmware smooths the input with a low-pass filter, so steps and reversals ramp.

## 6. Protections (the only limits)

| Protection | Value | Where |
|---|---|---|
| Hardware overcurrent break | TIMER0 BRKIN on PB12, AF2, pull-up, active low. Kills all six gates in hardware | `setup.c` (patched), same as stock (`CCHP = 0xD020`) |
| Dead time | 60 timer ticks (stock 32) | `DEAD_TIME`, upstream |
| Software current limit | **2.5 A per board** (24 V / 5 A supply, two boards). Over the limit: input ramps to 0, gates off below 100 | `DC_CUR_LIMIT`, `bldc.c` |
| Invalid hall state (0 or 7) | Gates off at once (sensor missing or broken) | `bldc.c` |
| Output off = real off | Clears POEN (upstream only cleared OAEN, which left the gates switching) | `overlay/Inc/bldcOutputs.h` |
| PWM smoothing filter | Ramps sudden changes (limits braking energy pushed into the converter) | upstream `FILTER_SHIFT` |

Not limited: speed (full ±1000), direction changes, run time, supply voltage.

## 7. Pin map (layout 2.3.2, identical on both boards)

| Function | Pin | Mode |
|---|---|---|
| Phase Y high / low | PA8 / PB13 | TIMER0 CH0 / CH0N, AF2 |
| Phase B high / low | PA9 / PB14 | TIMER0 CH1 / CH1N, AF2 |
| Phase G high / low | PA10 / PB15 | TIMER0 CH2 / CH2N, AF2 |
| Overcurrent break | PB12 | TIMER0 BRKIN, AF2, pull-up |
| Hall A / B / C | PB11 / PC14 / PA1 | input. State = PB11 + 2·PC14 + 4·PA1 (as stock) |
| DC current / Vbus | PA6 / PA4 | analog |
| Power latch (SELF_HOLD) | PB2 | output, high = stay on |
| Buzzer | PF0 | output (master only) |
| LEDs | PB3 green, PA15 orange, PB4 red, PF1 upper, PB5 lower | output |
| Master-slave link | PA2 TX / PA3 RX | USART1, AF1 |
| Debug | PA13 SWDIO / PA14 SWCLK | SWD |
| Not used | PA5 (button), PA7, PB0, PB6/PB7, PB8/PB9 | stock uses PA7/PB0 analog, PB8/PB9 output: unidentified |

## 8. Build, test, flash

```sh
cd swd-flasher
.venv/bin/python scripts/build_hoverboard.py master   # -> firmware/master.bin + master.build.txt
.venv/bin/python scripts/build_hoverboard.py slave    # -> firmware/slave.bin  + slave.build.txt
analysis/run_tests.sh                                  # 8 emulator suites, all must pass
make -C test test                                      # ESP32 SWD/mailbox simulator
```
Bench test of the slave through the master (both boards flashed, ESP32 in `CONTROL_MODE 1` on the master):
```sh
make bench-slave            # slave wheel ±100 (master wheel stays 0), then the LED sequence
make bench-slave-wheel SPEED=150
make bench-slave-leds
```
**Note:** `make bench-slave*` (`scripts/bench_slave.py`) talks to the current ESP32 console and will need to be modified to work with the final version of the ESP32 firmware.

The build refuses to write an image if the configuration, pins, mailbox contract or RAM layout are wrong.
Control mode: set `CONTROL_MODE 1` in `src/config.h`, build and flash the ESP32, connect connector 1 to the master's SWD header. The console reads commands from the USB port (USB-serial-JTAG driver).

Flashing a board with the ESP32: hold the board's power button during the whole flash. Erasing removes the firmware that holds the power latch (PB2), and the board switches itself off.

## 9. Known limits / to check on the bench

* **Wheel direction**: positive = forward is upstream's convention, not measured. If a wheel runs backwards, flip `DIR_MASTER` or `DIR_SLAVE`.
* **Current sense**: 0.2 A per ADC count, calibration unverified. Compare `st` current with the supply meter. Noise could trip the 2.5 A limit early.
* **Vbus**: scaling unverified (`ADC_BATTERY_VOLT`).
* **Braking energy**: the converter cannot absorb it. Avoid hard stops from high speed until a diode or brake resistor is fitted.
* **Hall pull resistors**: stock uses internal pull-downs on the hall inputs, upstream none. Not changed.
* **Power-up**: needs the button (hardware latch). Starting on power-on needs a hardware change.
* **Boot timing**: mailbox ready ≈170 ms after power-up (emulator).

## 10. Source map

| File | Role |
|---|---|
| `overlay/Inc/remoteDebugPort.h` | Mailbox layout, bits, timeouts, wheel direction |
| `overlay/Src/remoteDebugPort.c` | Master: mailbox, per-wheel commands, slave relay, telemetry, power off |
| `overlay/Src/slaveWatchdog.c` | Slave: 800 ms frame watchdog, LEDs from the frame |
| `overlay/Src/debugLed.c`, `Inc/debugLed.h` | LED driver; all upstream LED writes become no-ops |
| `overlay/Inc/bldcOutputs.h` | Real output off (POEN) |
| `scripts/build_hoverboard.py` | Copies upstream, applies every patch, builds, verifies |
| `src/mailbox.[ch]`, `src/control.c` | ESP32 mailbox client and console |
| `analysis/test_*.py`, `run_tests.sh` | Emulator tests of both images |
