"""Bench test of the SLAVE board through the MASTER's debug port: low-speed wheel run and LED sequence.

usage: .venv/bin/python scripts/bench_slave.py {wheel,leds,all} [--port /dev/cu.usbmodem101] [--speed 100]
       (or: make bench-slave / bench-slave-wheel / bench-slave-leds)

NOTE: this drives the ESP32 bench console of the CURRENT swd-flasher firmware (CONTROL_MODE 1 in
src/config.h, text commands `arm`, `s <master> <slave>`, `led ...`, `st`; see API.md). It will need
to be modified to work with the final version of the ESP32 firmware, whose command interface is
not settled yet.

Needs: master and slave flashed with firmware/master.bin and slave.bin, master on SWD connector 1,
master-slave cable connected, both boards on. The master wheel command is always 0: only the slave
wheel moves. The script always ends with the wheels stopped and disarmed, also on errors or Ctrl-C.
"""
import argparse
import re
import sys
import time

import serial

LEDS = ('green', 'orange', 'red', 'upper', 'lower')


class Console:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.read(65536)

    def cmd(self, line, wait=0.3, show=True):
        self.s.write((line + '\n').encode())
        self.s.flush()
        time.sleep(wait)
        out = [l.split('control: ', 1)[-1] for l in self.s.read(65536).decode(errors='replace').splitlines()
               if 'control: ' in l]
        if show:
            for l in out:
                print('    ' + l[:200])
        return out

    def status(self, label):
        print(f'-- {label}')
        return '\n'.join(self.cmd('st', 0.6))


def preflight(c):
    st = c.status('preflight')
    if 'STOPPED' not in st and 'DRIVING' not in st:
        sys.exit('no status from the ESP32: is it in CONTROL_MODE 1 and connected to the master?')
    if 'SLAVE-LINK-DOWN' in st:
        sys.exit('slave link down: check the master-slave cable and that the slave is on')
    m = re.search(r'slave:.*hall=(\S+)', st)
    if m and m.group(1) in ('?', '0!', '7!'):
        sys.exit(f'slave hall state {m.group(1)}: sensor missing or broken, not driving')


def odom(st):
    m = re.search(r'slave:.*odom=(-?\d+)', st)
    return int(m.group(1)) if m else None


def wheel(c, speed):
    try:
        start = odom(c.status('before'))
        c.cmd('arm')
        for sign, name in ((1, 'forward'), (-1, 'reverse')):
            c.cmd(f's 0 {sign * speed}', 1.5)
            c.status(f'slave {name} {sign * speed}, 1.5 s')
            time.sleep(1.5)
            c.status(f'slave {name} {sign * speed}, 3 s')
            c.cmd('stop', 1.5)
            end = odom(c.status('stopped'))
            if start is not None and end is not None:
                print(f'   => {name}: slave moved {end - start:+d} hall steps ({(end - start) * 4 / 360:+.1f} turns)')
                start = end
    finally:
        c.cmd('stop')
        c.cmd('disarm', 1.0)
        c.status('disarmed')


def leds(c):
    try:
        for n in LEDS:
            print(f'-- slave {n} ON (2 s)')
            c.cmd(f'led slave {n} on', 2.0, show=False)
            c.cmd(f'led slave {n} off', 0.5, show=False)
        print('-- slave ALL blinking 5x')
        for _ in range(5):
            c.cmd('led slave all on', 0.5, show=False)
            c.cmd('led slave all off', 0.5, show=False)
    finally:
        c.cmd('led slave all off', show=False)
        c.status('after LED test')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('test', choices=['wheel', 'leds', 'all'])
    ap.add_argument('--port', default='/dev/cu.usbmodem101')
    ap.add_argument('--speed', type=int, default=100, help='slave wheel command, 1..1000 (default 100 = low)')
    a = ap.parse_args()
    if not 1 <= a.speed <= 1000:
        sys.exit('--speed must be 1..1000')
    c = Console(a.port)
    try:
        preflight(c)
        if a.test in ('wheel', 'all'):
            wheel(c, a.speed)
        if a.test in ('leds', 'all'):
            leds(c)
    except KeyboardInterrupt:
        c.cmd('stop', show=False)
        c.cmd('disarm', show=False)
        sys.exit('interrupted: stopped and disarmed')


if __name__ == '__main__':
    main()
