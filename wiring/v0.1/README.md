# WireViz files — boundary robot

These diagrams are split into reusable electrical modules.

## Reused module
`triangulation_anchor.yaml` is intended to be instantiated 3 times:
- TRI-1
- TRI-2
- TRI-3

## Important assumptions
- The ESP32-C6 board is powered from a regulated 5 V rail through its USB/5V input, unless the exact board exposes a suitable VIN pin.
- ESP32-C3 anchor boards use 3.3 V logic.
- CC1101 modules are 3.3 V devices. Do **not** feed a bare CC1101 from 5 V.
- BTS7960 is used as one H-bridge per motor. For two drive motors, use two BTS7960 modules.
- The 775/NFP-RC-545SH blade motor is a high-current 12 V load; its exact current determines battery, fuse, wiring gauge, and BTS7960 suitability.
- The 14.6 V LiFePO4 charger is a charger input, not a logic supply.
- Copper charging plates are shown as the DC charging interface only.
- The relay file treats the LY2NJ/HH62P/HHC68A-2Z as a generic DPDT power relay. Verify the exact socket/terminal numbering on the purchased relay.
- The KW12-3 bumper switches are shown as simple dry-contact switches.
- NEO-6M UART pin names are functional labels; exact ESP32 GPIO assignment is intentionally left as GPIO_* placeholders.
- Pin numbers on generic modules can vary by board/vendor. Verify the silkscreen/datasheet before wiring.

Files:
- `robot_controller.yaml`
- `drive_motor.yaml`
- `blade_motor.yaml`
- `bumper_switch.yaml`
- `bts7960_motor_controller.yaml`
- `dpdt_relay.yaml`
- `charging_plate.yaml`
- `charging_input.yaml`
- `boundary_wire.yaml`
- `triangulation_anchor.yaml`
- `robot_gps_rf.yaml`
- `base_station_finder.yaml`
- `base_station.yaml`
- `sonic_sensor.yaml`

Non-electrical parts (acrylic dome, PETG filament, O-ring cord, QR code) are not useful as standalone WireViz diagrams.
