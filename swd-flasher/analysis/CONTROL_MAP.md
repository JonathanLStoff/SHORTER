# Decompiled stock vs our build: how the wheel and the LEDs are addressed

Checked on 2026-11-06 with `disassemble.py`, `pin_report.py`, `test_hall_order.py` (stock routines run
in the emulator). Addresses are stock image addresses (`../backup/*_stock.bin`).

## Drive (motor PWM)

| Item | Stock | Our build (upstream BLDC_SINE + our overlay) | Same? |
|---|---|---|---|
| Timer | TIMER0, complementary outputs, centre-aligned family (CTL0 0x61 after our init); stock frequency not confirmed (its init values are not final) | TIMER0, centre-aligned (CTL0 0x61), CAR 2250 = 16 kHz | likely |
| Yellow / blue / green high side | PA8 / PA9 / PA10 (AF2) = CH0 / CH1 / CH2 | PA8 / PA9 / PA10 (BLDC_YH / BH / GH) = TIMER_CH_0 / 1 / 2 | yes |
| Low sides | PB13 / PB14 / PB15 (AF2) = CH0N / CH1N / CH2N | PB13 / PB14 / PB15 (BLDC_YL / BL / GL) | yes |
| Duty write | routine 0x080055C0 stores CH0CV from its first argument, CH1CV from RAM 0x20000220, CH2CV from RAM 0x20000222, each clamped at 0 | `bldc.c`: `CHxCV = CLAMP(y/b/g + MID, 10, MAX)` | same registers |
| Channel order | CH0 -> PA8 (yellow), by the AF pin mapping | `TIMER_BLDC_CHANNEL_Y = TIMER_CH_0` -> PA8 (yellow) | yes (which stock phase CH0 is comes from its caller, not traced) |

Pin addressing is therefore the same: the three duties go to CH0/CH1/CH2, which are PA8/PA9/PA10 and
PB13/PB14/PB15 in both. What the stock does not show us is the sine table itself (we did not extract it).

## Hall sensors (position feedback)

| Item | Stock | Our build | Same? |
|---|---|---|---|
| Sensor pins | PC14, PB11, PA1 | HALL_A PC14, HALL_B PB11, HALL_C PA1 (upstream 2.3.2) | pins yes |
| State formula (routine 0x08004DD2) | `state = PB11 + 2*PC14 + 4*PA1` | `ReadHallState() = HALL_A + 2*HALL_B + 4*HALL_C` | **no: bits 0 and 1 swapped** |
| Invalid states 0 and 7 | treated as fault (checked at 0x08004DFA) | gates off (`pos == 0` in `CalculateBLDC`, POEN cleared) | yes |
| How read | polled GPIO input registers (no EXTI vectors in the stock vector table) | EXTI interrupts on the three pins, plus ReadHallState | different method |

**Fix applied:** the build sets `HALL_A = PB11` and `HALL_B = PC14` (`scripts/build_hoverboard.py`,
`patch_hall_order`). With the upstream formula that gives `state = PB11 + 2*PC14 + 4*PA1`, identical to
the stock. `test_hall_order.py` checks all eight sensor patterns against the stock routine: all match.

## Hall-driven test features (all off)

- `TEST_HALL2LED`: the hall-to-LED display in `bldc.c` is compiled out. Verified at build time.
- `REMOTE_AUTODETECT` (spins the wheel to find the hall pins): off. Verified at build time.
- `DEBUG_LED`, `RTT_REMOTE`, `REMOTE_UART/UARTBUS`: off. Verified at build time.
- No foot-pad / IMU / balance code in the image (`MPU_6050`, `SEND_IMU_DATA`, pilots: checked at build
  time; no I2C code linked). The main loop reads no input pins (`test_master_mailbox.py`).
- Hall sensors only set the commutation (EXTI + ReadHallState) and are reported to the debug port
  (`hallMaster`, `hallSlave`). Driving mode 0 (pwm): the hall speed never feeds back into the command.

## LEDs

| LED | Pin | Stock | Ours |
|---|---|---|---|
| Green | PB3 | output | debug port only (bit 0) |
| Orange | PA15 | output | debug port only (bit 1) |
| Red | PB4 | output | debug port only (bit 2) |
| Upper | PF1 | output | debug port only (bit 3) |
| Lower | PB5 | output | debug port only (bit 4) |

Stock: set/reset through the SPL helper (BOP/BC). The init leaves PB3, PB5, PA15 and PF0/1 low; the
battery-display code that drives the rest is not traced. Our build rewrites every upstream LED write to
`LedWrite()`, which does nothing; only `DebugLedApply()` drives the pins (master: mailbox `ledMaster`,
slave: frame LED bits). `test_leds.py` checks that every LED pin write since reset comes from it, on both images.

## Output enable and break

| | Stock | Ours |
|---|---|---|
| Outputs on | writes CCHP = 0xD020 (POEN, OAEN, BRKEN, break active low, dead time 32) at 0x08002570 / 0x08002dd4 | OAEN set; hardware sets POEN at the next update while the break is inactive. BRKEN set at init |
| Outputs off | writes CCHP = 0x0020 (POEN cleared) | POEN and OAEN cleared (`bldcOutputs.h`). Upstream cleared only OAEN, which left POEN set and the gates switching |
| Break input | PB12 AF2 pull-up | same (`TIMER_BLDC_EMERGENCY_SHUTDOWN PB12`, pull-up) |
| Dead time | 32 | 60 |

## Not matched / not yet explained

- PB8, PB9: stock outputs with no writes during init. Upstream uses them only for the optional I2C IMU
  (`I2C_PB8PB9`). Not identified, so not exposed as LEDs.
- PB6, PB7: stock USART0 (AF0). Not used by our build.
- Stock OCTL writes to PB13-15 (value 0xE000) in a routine near 0x08003DF8: role not identified.
- Hall input pulls: stock pull-down on PB11 / PC14 / PA1, ours none.
- PA7 and PB0: stock analog, ours unused.
