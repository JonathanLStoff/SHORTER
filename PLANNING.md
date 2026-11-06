# SHORTER planning

Living plan for the mower. Wiring: [`wiring/v0.3/`](wiring/v0.3) (rendered in `wiring/exported/`). Drive state: `swd-flasher/CURRENT_STATE.md`. Drive interface: `swd-flasher/API.md`.

## Future architecture

### Power
- One **Greenworks battery** runs everything:
  - both hoverboard boards and the blade motor driver, straight from the pack;
  - the ESP32-C6, through a buck converter to 5 V.
- **Battery:** a current Greenworks **40 V model is being modified**, so the final voltage and full-charge voltage are **TBD**. The planning value is 24 V.
- **Hoverboard power:** one battery connector splits into two wires per side, one +/− pair to the master and one to the slave.
- **Charging:**
  - copper plates on the mower and on the base station, with the Greenworks charger in the base station;
  - a relay on the mower (kept), so the plates are dead when the mower is off the dock, plus the base station's own relay.

### Drive (hoverboard)
- A split-board hoverboard drives the mower. The ESP32-C6 commands both wheels over the **master** board's SWD debug header, and the slave follows the master over the stock master-slave cable. Everything known about the boards, pins, power, protections and measured speed is in [`wiring/hoverboard.md`](wiring/hoverboard.md).
- **Baseline movement speed is command 100** (of ±1000, per wheel, positive = forward). This is the bench-tested low speed: about 0.5 wheel rev/s at 0.2–0.6 A, smooth in both directions (2026-11-06, slave wheel off the ground, 24 V). Re-run it with `make -C swd-flasher bench-slave-wheel`. Use it as the default cruise speed and derive slower or faster moves from it. Re-check it under load, on grass and on the Greenworks battery.
- The software current limit is 2.5 A per board, set for the 5 A bench supply. **Raise it for the battery.**

### Main controller
- **ESP32-C6.** GPIO0/1 are SWD to the hoverboard master, GPIO8 is the board's WS2812, and GPIO12/13 are native USB.
- The final ESP32 firmware replaces today's bench console. `make bench-slave*` will need to be modified for it.

### Sensors and peripherals
| Part | Status |
|---|---|
| NEO-6M GPS | keep |
| GY-521 MPU-6050 gyro | keep, wiring next |
| CC1101 915 MHz | keep, wiring next |
| 4x KW12-3 bumpers | keep, wiring next |
| Blade motor + BTS7960 blade driver | keep, choose a 24 V motor, wiring next |
| **HLK-LD2410D 24 GHz radar** | new: a presence sensor being retro-fitted to tell **grass from concrete**. Drawn in v0.3: 5 V, UART on GPIO17/18, OUT on GPIO19 (tentative). LD2410-family 5-pin header with 3.3 V logic; supply voltage and baud rate to confirm from the LD2410D datasheet |
| Status LED | wiring next (the dev board's WS2812 on GPIO8 is one option) |
| HC-SR04 ultrasonic | **dropped** |
| Boundary wire | **dropped** |
| GM37-3530 drive motors + 2x BTS7960 | **replaced** by the hoverboard |
| ESP32-CAM base finder | undecided, left as in v0.2 |

### Other modules
- **Triangulation anchors:** 3x ESP32-C3 + CC1101 (`wiring/v0.3/triangulation_module.yaml`). In the plan.
- **Base station:** ESP32-C3 + CC1101, the Greenworks charger, a relay to the copper plate, and a QR marker (`wiring/v0.3/base_station.yaml`). In the plan.

## Immediate next steps

Work out the wiring (pins, power, connectors) for each, then update `wiring/v0.3`:

1. **Gyro:** GY-521 MPU-6050 on I2C to the ESP32-C6. The hoverboard's own IMU is on the GD32 and not used.
2. **Blade motor and the Greenworks battery** that powers it. The listed 31ZY coreless motor is 24 V; the 775 / RS-545 options are 12 V.
3. **Presence sensor:** the HLK-LD2410D 24 GHz radar for grass vs concrete. The LD2410 family can report per-distance-gate signal energy over UART (engineering mode). That's the likely input for telling surfaces apart; the plain presence OUT pin probably isn't enough.
4. **Motor controller for the blade motor:** BTS7960 in v0.3. BTS7960 modules are typically rated to about 27 V, so this depends on the battery's final voltage. A stock 40 V-class pack would be too high.
5. **Bumper switches:** 4x KW12-3.
6. **CC1101** 915 MHz radio (SPI).
7. **Status LED.**

### Pin conflicts to resolve while doing this
v0.3 carries the peripheral pins over from v0.2; only the ones that clashed with the hoverboard link moved.
- GPIO0 is SWDIO now, so the charging relay moved to GPIO16 (freed by dropping the drive BTS7960s). A relay coil also needs a driver transistor; the ESP32 can't drive it directly.
- GPIO8: CC1101 GDO0 in v0.2/v0.3, but it's the WS2812.
- GPIO12/13: camera UART, but they're native USB D−/D+.
- GPIO24–28: blade PWM and bumpers. On the ESP32-C6 these are normally the SPI flash pins and aren't broken out on the DevKitC-1. Verify on the board.
- GPIO4, 5, 8, 9, 15 are strapping pins (CC1101 SI/SO/GDO0/GDO2, IMU SCL). Usable with care.
- Freed by v0.3: GPIO2, 3 (SWD connector 2, not needed now) and 20–23. GPIO17–19 are now the LD2410D (tentative).

## TODO (later)
- **Greenworks charging through the copper plates.** Greenworks packs and chargers may need the pack's extra contacts (ID / thermistor), not just + and −. Check this once the pack and charger models are settled.

## Open / TBD
1. **Battery voltage:** final and full-charge voltage of the modified 40 V Greenworks pack. It sets the BTS7960, buck and hoverboard input margins, the blade motor choice and the hoverboard current limit.
2. **LD2410D datasheet:** supply voltage (5 V vs 3.3 V), pin labels and default baud rate.

Settled 2026-11-06: keep the mower-side charging relay. The slave board is powered from its own pair of the split battery connector.
