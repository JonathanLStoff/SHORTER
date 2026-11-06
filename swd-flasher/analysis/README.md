# Firmware analysis (stock backups and our builds)

Host-side tools for the GD32E230 images. Nothing here touches a board.

| File | What it is |
|---|---|
| `master_stock.dis`, `slave_stock.dis` | Disassembly of the stock backups (`../backup/`). Linear sweep from 0xC0, Thumb. Each `ldr` from a literal pool shows the loaded word as `; =VALUE`, so peripheral bases are visible (e.g. `; =48000400` = GPIOB, `; =40012c34` = TIMER0 CH0CV) |
| `gd32emu.py` | Emulator (unicorn) with peripheral behaviour for RCU, ADC, USART status, GPIO ISTAT and hall states, and SysTick/timer ISR injection. Logs every GPIO/TIMER/ADC/USART write with its PC |
| `disassemble.py` | Makes the `.dis` listings |
| `pin_report.py` | Prints the pin configuration (mode, alternate function, pull) and TIMER0 setup after init. `--diff` compares two images |
| `hbtest.py` | Shared test helpers: mailbox offsets, frame encode/decode (CRC16), motor-input recorder |
| `run_tests.sh` | Runs every test below against `firmware/*.bin` |
| `test_master_mailbox.py` | Master: boot, per-wheel commands, relay to the slave (decoded USART1 frames), LEDs of both boards, 600 ms heartbeat timeout, enable, slave telemetry and hall, power off, no input-pin reads in the main loop |
| `test_slave_frames.py` | Slave: no mailbox, frames drive the motor, bad CRC, disable bit, 800 ms watchdog, LEDs from the frame, hall state in the reply, switch-off |
| `test_leds.py` | Either image: LEDs off at boot, masks drive the pins, every LED pin write since reset traced to DebugLedApply |
| `test_outputs.py` | Either image: break input (BRKEN, PB12 AF2 pull-up), dead time, gate pins, real output off (POEN) on disable / invalid hall / over 2.5 A |
| `test_hall_order.py` | Hall state of stock vs our build for all 8 sensor patterns (runs the stock routine at 0x08004DD2) |
| `CONTROL_MAP.md` | Decompiled stock vs our build: timer channels, pins, hall formula, LEDs, and what is not matched |

## Reproduce

```sh
cd swd-flasher
.venv/bin/pip install -r analysis/requirements.txt
.venv/bin/python analysis/disassemble.py backup/slave_stock.bin analysis/slave_stock.dis
.venv/bin/python analysis/pin_report.py backup/slave_stock.bin --diff firmware/slave.bin .build-hoverboard/slave/.pio/build/genericGD32E230C8/firmware.elf
analysis/run_tests.sh       # all emulator tests, both images
```
(The ELF files exist after `scripts/build_hoverboard.py <role>`.)

## What the stock images say (from `pin_report.py` on both backups, identical)

| Function | Pins | Stock configuration |
|---|---|---|
| Motor high side (TIMER0 CH0/1/2) | PA8 / PA9 / PA10 | AF2, yellow / blue / green |
| Motor low side (CH0N/1N/2N) | PB13 / PB14 / PB15 | AF2 |
| Hall sensors | PC14 (A), PB11 (B), PA1 (C) | input, **pull-down** |
| Battery / DC current | PA4 / PA6 | analog |
| Phase sense | PB0 | analog |
| Self-hold | PB2 | output |
| LEDs | PA15 orange, PB3 green, PB4 red, PB5 lower, PF1 upper | output |
| Buzzer | PF0 | output |
| Master-slave link | PA2 / PA3 (USART1) | AF1 |
| Break input (TIMER0 BKIN) | PB12 | AF2, pull-up. CCHP is 0x0020 at init; the stock writes 0xD020 (POEN, OAEN, BRKEN, active low) when it turns the outputs on (0x08002570, 0x08002dd4) and 0x0020 to turn them off |
| Unknown | PA7 analog, PB8 / PB9 output, PB6 / PB7 USART0 (AF0) | not in the 2.3.2 defines |

These match `Inc/defines/defines_2-3-2.h` of the upstream firmware for everything that layout defines.
