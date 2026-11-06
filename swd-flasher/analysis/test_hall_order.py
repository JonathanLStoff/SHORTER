"""Hall state: stock firmware vs our build, for all eight sensor patterns.

The stock routine at 0x08004DD2 reads the three hall inputs and stores the state at RAM 0x200001E4.
Our build calls ReadHallState(). Both run here on the same physical sensor levels.

usage: ../.venv/bin/python test_hall_order.py <stock.bin> <our.bin> <our.elf>
"""
import sys
from gd32emu import Emu

STOCK_HALL_ROUTINE = 0x08004DD2     # reads GPIOB ISTAT, GPIOA ISTAT, GPIOC ISTAT and forms the state
STOCK_STATE_RAM = 0x200001E4        # where the stock stores the state byte


def stock_state(stock_bin, a, b, c):
    emu = Emu(stock_bin).start()
    emu.set_physical_hall(a, b, c)
    from unicorn import UcError
    try:
        emu.uc.emu_start(STOCK_HALL_ROUTINE | 1, 0x08004DF6, timeout=0, count=200)   # stop address: no Thumb bit
    except UcError:
        pass
    return emu.uc.mem_read(STOCK_STATE_RAM, 1)[0]


def our_state(our_bin, our_elf, a, b, c):
    emu = Emu(our_bin, our_elf).start()
    emu.set_physical_hall(a, b, c)
    return emu.call(emu.syms['ReadHallState'])


def main(stock_bin, our_bin, our_elf):
    failures = 0
    print('PC14(A) PB11(B) PA1(C)   stock  ours')
    for a in (0, 1):
        for b in (0, 1):
            for c in (0, 1):
                s = stock_state(stock_bin, a, b, c)
                o = our_state(our_bin, our_elf, a, b, c)
                ok = s == o
                failures += not ok
                print(f'   {a}        {b}        {c}        {s}      {o}   {"ok" if ok else "MISMATCH"}')
    print('\nALL PASSED' if not failures else f'\n{failures} MISMATCHES')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
