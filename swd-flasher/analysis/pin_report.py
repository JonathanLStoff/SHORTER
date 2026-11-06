"""Run an image in the emulator and print its pin configuration and TIMER0 setup.

usage: ../.venv/bin/python pin_report.py <image.bin> [<image.elf>] [--diff <other.bin> <other.elf>]
"""
import sys
from gd32emu import Emu


def report(image, elf=None):
    emu = Emu(image, elf).start()
    return emu.gpio_state(), emu.timer0()


def main(argv):
    diff = None
    if '--diff' in argv:
        i = argv.index('--diff')
        diff = argv[i + 1:i + 3]
        argv = argv[:i]
    a_pins, a_tim = report(argv[1], argv[2] if len(argv) > 2 else None)
    if diff is None:
        for pin, (mode, pull) in a_pins.items():
            if mode != 'IN' or pull:
                print(f'{pin}:{mode}/pu{pull}', end='  ')
        print('\nTIMER0', {k: hex(v) for k, v in a_tim.items()})
        return
    b_pins, b_tim = report(diff[0], diff[1] if len(diff) > 1 else None)
    for pin in sorted(set(a_pins) | set(b_pins), key=lambda p: (p[1], int(p[2:]))):
        if a_pins.get(pin) != b_pins.get(pin):
            print(f'{pin}: {argv[1]}={a_pins.get(pin)}  {diff[0]}={b_pins.get(pin)}')
    print('TIMER0 differs' if a_tim != b_tim else 'TIMER0 identical', {k: hex(v) for k, v in b_tim.items()})


if __name__ == '__main__':
    main(sys.argv)
