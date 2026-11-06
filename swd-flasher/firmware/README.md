# Controller firmware images

`master.bin` and `slave.bin` are built by `scripts/build_hoverboard.py` (see `*.build.txt` next to
each one for the commit, layout, sha256 and mailbox placement). Do not hand-edit them.

Full interface: `../API.md`.

| Image | Role | Control |
|---|---|---|
| `master.bin` | MASTER, layout E230 2 (2.3.2) | RAM mailbox v2 (`MBX2`) at 0x20001800, written by the ESP32 over SWD (`overlay/Src/remoteDebugPort.c`). One command per wheel, LEDs of both boards, power off. Drives its wheel and relays the slave wheel + slave LEDs over USART1 (PA2/PA3). Stops both wheels 600 ms after the last heartbeat |
| `slave.bin` | SLAVE, layout E230 2 | No mailbox. Only master frames over USART1: wheel, LEDs, disable, switch off. 800 ms frame watchdog. Replies with current, speed, odometry and its hall state |

Both images:
- No foot-pad / IMU / balance code. The main loop reads no input pins.
- Hall sensors only commutate the motor and are reported. Hall order `HALL_A = PB11`, `HALL_B = PC14`,
  `HALL_C = PA1`, the stock order (`analysis/CONTROL_MAP.md`).
- LEDs (green PB3, orange PA15, red PB4, upper PF1, lower PB5) are driven only from the debug port.
  Off at boot.
- No inactivity, low-battery or button power-off, no beeps except one 150 ms boot beep (master).
- Protections: hardware overcurrent break (PB12, as stock), dead time 60, 2.5 A software limit per
  board, gates off on an invalid hall state, and "off" now really clears POEN.

Build: `.venv/bin/python scripts/build_hoverboard.py master` (or `slave`). The build fails unless the
compiler sees the intended role, layout, pins and protections, and the mailbox contract matches.
Tests: `analysis/run_tests.sh`.

## Flashing (not done yet: needs an explicit go-ahead, one board at a time)

The flasher only embeds `target_a.bin`. Copy the image there, build the ESP32 with `pio run`, plug in
exactly one board, and remove `target_a.bin` again before the next board. Restore stock with
`../backup/master_stock.bin` or `../backup/slave_stock.bin`.

The slave currently on the board is **not** this image: it has the earlier build, which was SINGLE
with the REMOTE_DUMMY zigzag test (and layout 2.3.1). See `../CURRENT_STATE.md`.

## Why the old images were wrong

The build script edited `configDebug.h`, but the compiler reads `config.h` unless `CONFIG_DEBUG` is
defined. So the "slave" was a SINGLE board on the zigzag test remote with layout 2.3.1. Fixed in the
script: `CONFIG_DEBUG` is enabled and the build checks the preprocessed macros.
