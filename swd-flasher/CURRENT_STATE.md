# Current state (updated 2026-11-06, debug-port control design v2)

Read this first when coming back. The full how-to and history are in the top-level `README.md`
("ESP32-C6 flasher / controller").

## Where everything stands

| Thing | State |
|---|---|
| **Master board** (power button, UID `4337485409544c573453827e`) | **Flashed 2026-11-06 with `firmware/master.bin` v2** (sha256 `257906d9…`); mailbox found. Flashing needs the power button held: erasing kills the PB2 self-hold. Backup `backup/master_stock.bin` (CRC32 `a790a40c`) |
| **Slave board** (UID `4337485416544c5734538631`) | **Flashed 2026-11-06 with `firmware/slave.bin` v2** (sha256 `ec382df5…`), programmed and verified. Bench 2026-11-06: wheel ran ±100 through the master (telemetry: ±0.5 rev/s, ≤0.6 A), LED sequence sent; visual check pending. Backup `backup/slave_stock.bin` (CRC32 `6dde5979`) |
| `firmware/master.bin` | New MASTER image, mailbox v2: both wheels + both boards' LEDs + power off over SWD. Not flashed |
| `firmware/slave.bin` | New SLAVE image: orders only from the master frame, 800 ms watchdog. **On the slave board** |
| ESP32-C6 | Control mode (`CONTROL_MODE 1`), no image embedded: cannot write flash. USB console input fixed (reads the USB-serial-JTAG driver). Control mode exists (`CONTROL_MODE 1` in `src/config.h`, default 0), master only. Not flashed |

**Interface reference: `API.md`.**

## What was wrong with the slave image that is on the board

The build script edited `configDebug.h`, but the compiler only reads it when `CONFIG_DEBUG` is defined.
Without it, `config.h` was used, which gives: **SINGLE** (REMOTE_DUMMY = the built-in zigzag test that
moves the wheel back and forth), **layout 2.3.1** (`LAYOUT 1`), and no master link. The
shaking wheel is almost certainly this zigzag test. The earlier theory about the hall order (2.3.1 vs
2.3.2) is not needed to explain it, and the pins were the wrong layout's, not 2.3.2.

Also found: on GD32E230 the upstream timeout timer is never started (`TimeoutTimer_init` is skipped
for TARGET 3), so `ResetTimeout()` does nothing. A slave with a dead master link would keep driving
its last PWM. The new slave has its own watchdog (now 800 ms) (`overlay/Src/slaveWatchdog.c`).

## Design v2 (2026-11-06, agreed in the review; verified in emulation, not on hardware)

- One debug link: everything goes through the **master's** SWD port. The slave has no mailbox; it gets
  its wheel command, LEDs, disable and switch-off in the master frame (USART1).
- Per-wheel commands `cmdMaster` / `cmdSlave`, -1000..1000, positive = forward. Upstream speed/steer
  mix bypassed. ESP32 console: `s <master> [slave]`, full range.
- LEDs of both boards from the master's port (`led master|slave|both <name> on|off`). Firmware never
  drives LEDs (no battery display). Upstream bugs fixed: `wStateSlave & !STATE_Disable` wiped every bit;
  the lower-LED line wrote UPPER_LED (now irrelevant: all upstream LED writes are no-ops).
- Halls: commutation only, reported as `hallMaster` / `hallSlave` (slave sends it in reply bits 4..6).
  No foot-pad/IMU code; the main loop reads no input pins (`test_master_mailbox.py`).
- Timeouts: heartbeat 600 ms (master stops both), slave frame watchdog 800 ms.
- Removed: inactivity, low-battery and button power-off, reverse beep, battery checks, startup melody
  (one 150 ms beep instead). Button only boots (hardware latch). `off` command switches both off.
- Protections: TIMER0 break input PB12 enabled (stock writes CCHP = 0xD020 = POEN|OAEN|BRKEN when it
  turns the outputs on), dead time 60, `DC_CUR_LIMIT` 2.5 A per board (24 V / 5 A supply), gates off on
  invalid hall. **Upstream "outputs off" only cleared OAEN, which leaves POEN set and the gates
  switching**; the build now clears POEN (`overlay/Inc/bldcOutputs.h`).
- Tests: `analysis/run_tests.sh` (8 suites, both images) and `make -C test test`: all pass.

## Open items

1. **Flash the new images?** Needs your go-ahead, one board at a time. Master first, on the 24 V supply,
   wheels off the ground, low commands first (`s 100 100`).
2. Bench checks: wheel direction (`DIR_MASTER` / `DIR_SLAVE`), current reading vs the supply meter
   (0.2 A per ADC count, the 2.5 A limit may trip on noise), Vbus scaling.
3. Not matched to stock: hall input pull-downs (stock pull-down, ours none), PA7 / PB0 analog and PB8 / PB9
   outputs (stock, unidentified), PWM 16 kHz vs stock ~17.6 kHz.
4. Power-on without the button needs a hardware change to the latch.
5. Braking energy goes back into the converter; a diode or brake resistor before hard stops at speed.

## Hard-won facts about this hardware
- **GD32E230 flash must be programmed in 32-bit words.** Halfword writes partly work, but some fail
  with PGAERR (FMC_STAT bit 3); 74 of ~15,000 were silently lost before this was fixed.
  Option bytes too: one 32-bit write of `0xFFFF5AA5` to `0x1FFFF800`
- **RDBUFF is valid only on the first read** after an AP read; repeat reads return 0
- **ESP32 SWD pins need the weakest drive (0)**: default drive strength rings on the wires and the
  GD32 returns garbage (DPIDR `0xfc0f801b` instead of `0x0bf11477`)
- Debug header order from the 3.3V end: 3.3V, SWCLK, SWDIO, GND. Leave 3.3V unconnected
- Hoverboard must be switched on; when off, the SWD pins half-power the chip (CPUID reads 0)
- SW-DP v1: no multidrop, so master and slave cannot share the same two ESP32 pins
- Both chips: Cortex-M23, 64 KB flash, not read-protected, stock firmware uses 0xF000 bytes
- **ESP32-C6: only flash through the native USB port** (`/dev/cu.usbmodem101`, "USB JTAG/serial
  debug unit"). The UART port's bridge corrupts uploads (that erased the bootloader once).
  If the LED is dark and the log says `boot:0x4 DOWNLOAD`, unplug USB for 5 s or just reflash
- ESP32-C6 tick is 100 Hz: `vTaskDelay(pdMS_TO_TICKS(1))` is 0 ticks and doesn't yield

## Resume checklist
- `ls /dev/cu.usbmodem*`, then `.venv/bin/pio device monitor -p /dev/cu.usbmodem101` to see what the ESP32 sees
- `cd test && make test`: host simulator (models E230 word programming, RDBUFF, lost writes)
- `scripts/build_hoverboard.py <role> [--layout N]` builds firmware without touching the submodule
- `swd-flasher/fw_empty/` (two empty .bin files) was not created by Claude; check what it is for
