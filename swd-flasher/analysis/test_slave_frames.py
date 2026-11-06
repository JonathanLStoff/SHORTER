"""Emulated test of the SLAVE: its only command source is the master frame over USART1.

usage: ../.venv/bin/python test_slave_frames.py <slave.bin> <slave.elf>
Frame (commsMasterSlave.c, SerialMaster2Slave): '/', int16 pwm, uint8 wState, uint16 CRC16(0x1021).
wState bits 0..4 = LEDs, 0x40 = disable, 0x80 = switch off. What the motor gets is read from the
arguments of SetBldcInput; the slave's replies are decoded from its USART1 bytes.
"""
import sys

from gd32emu import Emu
from hbtest import (MB, WS_DISABLE, WS_SHUTOFF, Checks, master_frame, slave_replies, feed_uart,
                    record_motor_inputs, byte, leds, mask_to_leds)


def main(image, elf):
    emu = Emu(image, elf)
    check = Checks()
    motor = record_motor_inputs(emu)
    s = emu.syms

    def frames(ms, pwm, ws=0, period=10):
        for t in range(ms):
            if t % period == 0:
                feed_uart(emu, master_frame(pwm, ws))
            emu.systick(1)

    emu.systick(600)
    check(motor and all(v == 0 for v in motor[-20:]), 'no master: motor input 0')
    check(leds(emu) == mask_to_leds(0), 'no master: LEDs off')
    check(emu.rd32(MB) == 0, 'slave has no debug mailbox (0x20001800 untouched)')

    # ---- motor from frames
    frames(200, 250)
    check(motor[-1] == 250 and byte(emu, s['enable']) == 1, f'frames pwm 250: motor input {motor[-1]}, enabled')
    frames(100, -1000)
    check(motor[-1] == -1000, 'frames pwm -1000: full range')
    feed_uart(emu, master_frame(-120, bad=True))
    emu.systick(20)
    check(motor[-1] == -1000, 'bad CRC: frame rejected, input unchanged')
    frames(100, 250, WS_DISABLE)
    check(byte(emu, s['enable']) == 0 and byte(emu, s['bldc_enable']) == 0, 'disable bit: motor disabled')
    frames(100, 250)
    check(byte(emu, s['enable']) == 1, 'disable bit cleared: enabled again')

    # ---- frame watchdog 800 ms
    del motor[:]
    emu.systick(700)
    check(motor and motor[-1] == 250, 'master silent 700 ms: still driving (timeout is 800 ms)')
    emu.systick(200)
    check(motor[-1] == 0, 'master silent 900 ms: watchdog stops the motor')
    frames(50, -300)
    check(motor[-1] == -300, 'frames again: pwm -300 accepted')

    # ---- LEDs from the frame
    for m in (0x1F, 0x05, 0x0A, 0x10, 0):
        frames(30, 0, m)
        check(leds(emu) == mask_to_leds(m), f'frame LED bits {m:#04x} -> {leds(emu)}')

    # ---- hall state in the reply
    emu.uc.mem_write(s['offsetcount'], (1000).to_bytes(2, 'little'))
    for a, b, c in ((1, 1, 0), (0, 0, 1)):
        emu.set_physical_hall(a, b, c)
        expect = emu.call(s['ReadHallState'])
        emu.call(s['CalculateBLDC'], 20000)
        n = len(emu.log)
        frames(20, 0)
        rep = slave_replies(emu, n)
        check(rep and rep[-1][0] & 0x80 and (rep[-1][0] >> 4) & 7 == expect,
              f'reply wState {rep[-1][0] if rep else None:#04x}: hall {expect} in bits 4..6, bit 7 set')

    # ---- switch off
    lv = emu.gpio_levels()['PB2']
    frames(30, 0, WS_SHUTOFF)
    check(lv == 1 and emu.gpio_levels()['PB2'] == 0, 'shutoff bit: SELF_HOLD (PB2) released')

    return check.done()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
