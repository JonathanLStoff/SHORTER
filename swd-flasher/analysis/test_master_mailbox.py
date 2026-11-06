"""Emulated test of the MASTER: debug-port mailbox, per-wheel commands, relay to the slave,
LEDs of both boards, timeouts, slave telemetry and power off. The ESP32 is played by writing RAM.

usage: ../.venv/bin/python test_master_mailbox.py <master.bin> <master.elf>
Virtual time: 1 ms = one SysTick_Handler call. What the master motor gets is read from the
arguments of SetBldcInput; what the slave gets is decoded from the USART1 bytes.
"""
import sys

from gd32emu import Emu
from hbtest import (MB, MAGIC, OFF, POWER_OFF_KEY, FW_DRIVING, FW_STOPPED, FW_HB_SEEN, FW_SLAVE_LINK,
                    WS_DISABLE, WS_SHUTOFF, Checks, master_frames, slave_reply, feed_uart,
                    record_motor_inputs, s32, f32, leds, mask_to_leds)


def main(image, elf):
    emu = Emu(image, elf)
    check = Checks()
    motor = record_motor_inputs(emu)
    hb = [0]

    def mb(name):
        return emu.rd32(MB + OFF[name])

    def put(name, v):
        emu.wr32(MB + OFF[name], v)

    def drive(ms, cm, cs, en=1, period=50):
        """ESP32 playing: command frame every `period` ms (heartbeat written last)."""
        for t in range(ms):
            if t % period == 0:
                put('cmdMaster', cm & 0xFFFFFFFF)
                put('cmdSlave', cs & 0xFFFFFFFF)
                put('enable', en)
                hb[0] += 1
                put('heartbeat', hb[0])
            emu.systick(1)

    # ---- boot
    t_magic = None
    for t in range(0, 1500, 10):
        emu.systick(10)
        if emu.rd32(MB) == MAGIC:
            t_magic = t + 10
            break
    check(t_magic is not None and t_magic < 600, f'mailbox magic MBX2 written {t_magic} ms after reset (short boot beep, no melody)')
    emu.systick(100)
    check(mb('fwState') & FW_STOPPED and not mb('fwState') & FW_HB_SEEN, 'before any heartbeat: stopped')
    check(leds(emu) == mask_to_leds(0), f'LEDs off at boot (no battery display): {leds(emu)}')
    fr = master_frames(emu)
    check(fr and all(p == 0 and w & WS_DISABLE for p, w in fr[-5:]), 'frames to the slave: pwm 0 and disable bit')

    # ---- main loop reads no input pins (no button, no foot pads; halls are read only in the motor ISR)
    for k in [k for k in emu.reads if k.endswith('ISTAT')]:
        emu.reads[k] = 0
    emu.systick(300)
    istat = {k: v for k, v in emu.reads.items() if k.endswith('ISTAT') and v}
    check(not istat, f'main loop + SysTick read no GPIO inputs in 300 ms: {istat or "none"}')
    emu.call(emu.syms['ReadHallState'])     # positive control: the read counter does see input reads
    check(sum(v for k, v in emu.reads.items() if k.endswith('ISTAT')) == 3, 'control: ReadHallState reads 3 inputs')

    # ---- per-wheel drive, opposite directions
    n = len(emu.log)
    del motor[:]
    drive(300, 300, -150)
    check(motor and motor[-1] == -300, f'cmdMaster 300 -> master SetBldcInput {motor[-1] if motor else None} (DIR_MASTER -1)')
    fr = master_frames(emu, n)
    check(fr and fr[-1][0] == -150 and not fr[-1][1] & WS_DISABLE,
          f'cmdSlave -150 -> slave frame pwm {fr[-1][0] if fr else None}, enabled')
    check(mb('fwState') & (FW_DRIVING | FW_HB_SEEN) == FW_DRIVING | FW_HB_SEEN, 'fwState: driving, heartbeat seen')
    check(mb('echo') == hb[0], 'heartbeat echoed')
    drive(200, -1000, 1000)
    fr = master_frames(emu, n)
    check(motor[-1] == 1000 and fr[-1][0] == 1000, 'full range: master -1000 -> input 1000, slave 1000 -> frame 1000')
    drive(100, 5000, -5000)
    fr = master_frames(emu, n)
    check(motor[-1] == -1000 and fr[-1][0] == -1000, 'out of range 5000/-5000 clamps to 1000/-1000')
    drive(100, 0, 400)
    fr = master_frames(emu, n)
    check(motor[-1] == 0 and fr[-1][0] == 400, 'one wheel only: master 0, slave 400 (no speed/steer mixing)')

    # ---- LEDs of both boards (no heartbeat needed)
    put('ledMaster', 0x0A)                  # orange + upper
    put('ledSlave', 0x15)                   # green + red + lower
    n = len(emu.log)
    drive(100, 0, 400)
    check(leds(emu) == mask_to_leds(0x0A), f'ledMaster 0x0A -> master pins {leds(emu)}')
    fr = master_frames(emu, n)
    check(fr and (fr[-1][1] & 0x1F) == 0x15 and not fr[-1][1] & 0x20,
          f'ledSlave 0x15 -> slave frame wState {fr[-1][1]:#04x} (LED bits, battery-display bit clear)')
    put('ledMaster', 0x1F)
    emu.systick(50)
    check(leds(emu) == mask_to_leds(0x1F), 'ledMaster 0x1F -> all 5 master LEDs on')
    put('ledMaster', 0)
    emu.systick(50)
    check(leds(emu) == mask_to_leds(0), 'ledMaster 0 -> all off')

    # ---- heartbeat timeout 600 ms
    drive(100, 250, 250)
    del motor[:]
    n = len(emu.log)
    emu.systick(500)                        # ESP32 silent, enable still 1
    check(motor and motor[-1] == -250, 'heartbeat silent 500 ms: still driving (timeout is 600 ms)')
    emu.systick(200)
    fr = master_frames(emu, n)
    check(motor[-1] == 0 and fr[-1][0] == 0 and fr[-1][1] & WS_DISABLE and mb('fwState') & FW_STOPPED,
          'heartbeat silent 700 ms: both wheels 0, slave disabled')

    # ---- enable 0
    drive(100, 250, 250)
    drive(100, 250, 250, en=0)
    fr = master_frames(emu)
    check(motor[-1] == 0 and fr[-1][0] == 0 and fr[-1][1] & WS_DISABLE, 'enable 0 with fresh heartbeat: both stop')

    # ---- slave telemetry through the master
    feed_uart(emu, slave_reply(0x80 | (5 << 4), 1.25, 3.5, 42))
    emu.systick(30)
    check(abs(f32(emu, MB + OFF['currentSlave']) - 1.25) < 1e-6 and abs(f32(emu, MB + OFF['speedSlave']) - 3.5) < 1e-6
          and s32(mb('odomSlave')) == 42, 'slave reply -> currentSlave 1.25 A, speedSlave 3.5, odomSlave 42')
    check(mb('hallSlave') == 5 and mb('slaveRx') == 1 and mb('fwState') & FW_SLAVE_LINK,
          f'slave hall {mb("hallSlave")}, replies {mb("slaveRx")}, slave link ok')
    emu.systick(900)
    check(not mb('fwState') & FW_SLAVE_LINK, 'no slave reply for 900 ms: slave link bit cleared')

    # ---- master hall telemetry (hall read by the motor routine)
    off_cnt = emu.syms['offsetcount']
    emu.uc.mem_write(off_cnt, (1000).to_bytes(2, 'little'))
    for a, b, c in ((1, 0, 0), (0, 1, 1)):
        emu.set_physical_hall(a, b, c)
        expect = emu.call(emu.syms['ReadHallState'])
        emu.call(emu.syms['CalculateBLDC'], 20000)
        emu.systick(30)
        check(mb('hallMaster') == expect, f'hallMaster reports {mb("hallMaster")} (ReadHallState {expect})')

    # ---- power off
    n = len(emu.log)
    lv_before = emu.gpio_levels()['PB2']
    del motor[:]
    put('power', POWER_OFF_KEY)
    emu.systick(200)
    fr = master_frames(emu, n)
    shut = [f for f in fr if f[1] & WS_SHUTOFF]
    check(len(shut) >= 5 and all(p == 0 for p, _ in shut), f'power off: {len(shut)} shutoff frames to the slave, pwm 0')
    check(lv_before == 1 and emu.gpio_levels()['PB2'] == 0, 'power off: SELF_HOLD (PB2) released')
    check(motor and all(v == 0 for v in motor), 'power off: master motor input 0')

    return check.done()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
