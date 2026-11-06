# Hoverboard drive: what we know

The mower's drive is a split-board ("Gen2") hoverboard: two motor controller boards, each driving one hub motor. The ESP32-C6 drives both through the **master** board's SWD debug header. Wiring: `v0.3/mower_module.yaml`. Firmware interface: [`../swd-flasher/API.md`](../swd-flasher/API.md). Current state: [`../swd-flasher/CURRENT_STATE.md`](../swd-flasher/CURRENT_STATE.md).

Status legend: **measured** = seen on our hardware; **from firmware** = read from the stock dump or the upstream layout file; **unverified** = assumed, check before relying on it.

## Boards

| | Master | Slave |
|---|---|---|
| Role | Has the power button. Takes commands from the ESP32 over SWD, drives its wheel, relays the slave's wheel + LEDs | Takes orders only from the master over the master-slave cable |
| Chip UID (measured) | `4337485409544c573453827e` | `4337485416544c5734538631` |
| Firmware (measured, 2026-11-06) | `swd-flasher/firmware/master.bin` (sha256 `257906d9…`) | `swd-flasher/firmware/slave.bin` (sha256 `ec382df5…`) |
| Stock backup | `swd-flasher/backup/master_stock.bin` (CRC32 `a790a40c`) | `swd-flasher/backup/slave_stock.bin` (CRC32 `6dde5979`) |

Both boards (measured / from firmware):
- MCU: GigaDevice **GD32E230C8**. That's a Cortex-M23 at 72 MHz, with 64 KB flash and 8 KB RAM. CPUID `0x411cd200`, DPIDR `0x0bf11477`, not read-protected. Stock firmware uses 0xF000 bytes.
- The pin layout is **2.3.2** of the RoboDurden Gen2.x-GD32 firmware (`defines_2-3-2.h`). Both boards use the same layout.
- The firmware base is RoboDurden Hoverboard-Firmware-Hack-Gen2.x-GD32 at commit c63b29a, plus our overlay (`swd-flasher/overlay`, built by `swd-flasher/scripts/build_hoverboard.py`).

## Connectors

| Connector | Pins | Notes |
|---|---|---|
| SWD debug header (4-pin) | from the 3.3 V end: **3.3V, SWCLK, SWDIO, GND** (measured) | Leave 3.3 V unconnected. SW-DP v1: no multidrop, so each board needs its own two pins. The ESP32 must use drive strength 0, or ringing corrupts the link |
| Master-slave cable | USART1: PA2 TX, PA3 RX (from firmware) | The stock cable, 115200 8N1. The other wires in it (power?) are **unverified** |
| Hub motor phases | 3 thick wires per board → PA8/PB13, PA9/PB14, PA10/PB15 gate pairs (from firmware) | Phase colours yellow / blue / green in the firmware |
| Hall sensor cable (per motor) | 3 signals: hall A PB11, B PC14, C PA1, plus supply and GND (signals from firmware; cable pinout **unverified**) | State = PB11 + 2·PC14 + 4·PA1 (the stock formula). Valid states are 1–6 |
| Power input | battery + / − on each board (measured) | One battery connector splits into two wires per side: one +/− pair goes to the master and one to the slave. On the bench both boards ran from a 24 V / 5 A DC supply |
| Power button | master only | Boots the board through a hardware latch |
| Foot-pad sensors, onboard IMU | not used | The stock firmware had them; our firmware has no code for them. IMU on I2C PB8/PB9 at 0x68 (upstream: not MPU-6050 compatible) |

## MCU pin map (from firmware, identical on both boards)

| Function | Pin |
|---|---|
| Phase Y high / low | PA8 / PB13 (TIMER0 CH0 / CH0N) |
| Phase B high / low | PA9 / PB14 (CH1 / CH1N) |
| Phase G high / low | PA10 / PB15 (CH2 / CH2N) |
| Overcurrent break input | PB12 (TIMER0 BRKIN, active low, pull-up) |
| Halls A / B / C | PB11 / PC14 / PA1 |
| DC current sense / Vbus | PA6 / PA4 (analog) |
| Power latch (self-hold) | PB2, high = stay on |
| Buzzer | PF0 (master) |
| LEDs | PB3 green, PA15 orange, PB4 red, PF1 upper, PB5 lower |
| Master-slave link | PA2 TX / PA3 RX |
| SWD | PA13 SWDIO / PA14 SWCLK |
| Unidentified (stock uses them) | PA7, PB0 analog; PB8/PB9 outputs; PB6/PB7 USART0 |

## Power and power-up

- **Supply.** We tested on a 24 V / 5 A DC converter. The planned supply is a Greenworks battery (a modified 40 V model, final voltage TBD), which also feeds the blade motor. The stock pack was 36 V (10 cells), so the boards run at 24 V. Vbus reads 22.9 V on the 24 V supply; that reading is uncalibrated.
- **Power-up.** The power button closes a latch, and the firmware then holds it on through PB2. The board can't start by itself when power is applied; that would need a hardware change to the latch.
- **Flashing.** Hold the power button the whole time. Erasing removes the firmware that holds PB2, and the board switches itself off.
- **Off.** Send the debug-port `off` command (master and slave), or remove power. The button is ignored after boot.
- **Half-powered chip.** If a board is off, the ESP32's SWD pins half-power the chip: the debug port answers but every read is 0.

## Motors and speed (measured on the slave wheel, off the ground, 24 V)

- Brushless hub motors with 3 hall sensors. One hall step is 4° of wheel rotation, so 90 steps per revolution (from firmware).
- **Baseline movement speed is command 100** (of ±1000). It gave about 0.5 rev/s, 0.2–0.6 A, and a smooth run forward and reverse (about 2.2 turns in 3 s). The wheel diameter hasn't been measured; at 6.5" (unverified) that's about 0.26 m/s.
- Bench test: `make -C swd-flasher bench-slave-wheel`.
- Direction: positive = forward is the firmware convention. Whether it pushes the mower forward is **unverified**.

## Protections in our firmware

| Protection | Value |
|---|---|
| Hardware overcurrent break | PB12, kills all six gates (same as stock) |
| Software current limit | 2.5 A per board. Set for the 5 A bench supply; **raise it for the Greenworks battery** (`DC_CUR_LIMIT_A` in `build_hoverboard.py`). Resolution is 0.2 A per ADC count, uncalibrated |
| Dead time | 60 timer ticks (stock 32) |
| Invalid hall state (0 or 7) | Gates off |
| Command heartbeat | Both wheels stop 600 ms after the last ESP32 heartbeat |
| Slave link | The slave stops its wheel 800 ms after the last master frame |
| PWM smoothing | Sudden changes and reversals ramp |

There is no battery, inactivity or button shutdown and no balance code. The firmware never moves a wheel on its own.

## Telemetry (per wheel, via the master mailbox)

Current (A), speed (rev/s), odometry (hall steps), hall state, Vbus, slave link status. Every 10 ms. Details: [`API.md`](../swd-flasher/API.md) §3.

## Unknown / to verify

- The master-slave cable pinout beyond TX/RX.
- The hall cable pinout and supply voltage at the connector.
- The wheel diameter, and which direction is forward on the mower.
- Current and Vbus calibration.
- What the stock firmware used PA7, PB0, PB8 and PB9 for (probably the foot pads and IMU).
- The Greenworks battery's final and full-charge voltage (a modified 40 V model, TBD). It must stay within the boards' input rating. The stock 36 V pack charged to about 42 V.
