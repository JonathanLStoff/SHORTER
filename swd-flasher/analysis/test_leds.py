"""LEDs belong to the debug port: from reset on, every write to an LED pin comes from DebugLedApply.
Upstream LED logic (boot blink, battery display, status) must never reach a pin.

usage: ../.venv/bin/python test_leds.py <image.bin> <image.elf>     (master or slave image)
master: LEDs follow mailbox ledMaster.  slave: LEDs follow the LED bits of the master frame.
Callers are identified by LR at the GPIO register write (digitalWrite -> gpio_bit_write).
"""
import sys

from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

from gd32emu import Emu, name_of
from hbtest import MB, OFF, Checks, master_frame, feed_uart, leds, mask_to_leds

# LED pins per GPIO port base: bit masks
LED_BITS_BY_PORT = {0x48000000: 1 << 15, 0x48000400: (1 << 3) | (1 << 4) | (1 << 5), 0x48001400: 1 << 1}


def main(image, elf):
    emu = Emu(image, elf)
    check = Checks()
    master = 'RemoteUpdate' in emu.syms
    lo = emu.syms['DebugLedApply']
    hi = min(a for a in emu.syms.values() if a > lo)
    callers = []

    def on_write(uc, acc, addr, size, val, ud):
        base, off = addr & ~0x3FF, addr & 0x3FF
        bits = LED_BITS_BY_PORT.get(base)
        if not bits or off not in (0x14, 0x18, 0x28):     # OCTL, BOP, BC
            return
        touched = (val | (val >> 16)) & 0xFFFF if off == 0x18 else val & 0xFFFF
        if off == 0x14 or touched & bits:
            callers.append((uc.reg_read(UC_ARM_REG_LR) & ~1, uc.reg_read(UC_ARM_REG_PC)))

    emu.uc.hook_add(UC_HOOK_MEM_WRITE, on_write, None, 0x48000000, 0x48001FFF)

    def set_mask(m, ms=60):
        if master:
            emu.wr32(MB + OFF['ledMaster'], m)
            emu.systick(ms)
        else:
            for t in range(ms):
                if t % 10 == 0:
                    feed_uart(emu, master_frame(0, m))
                emu.systick(1)

    emu.systick(1500)                       # boot and 1 s of main loop, no LED command
    check(leds(emu) == mask_to_leds(0), f'{"master" if master else "slave"}: LEDs off after boot: {leds(emu)}')
    for m in (0x01, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x13, 0):
        set_mask(m)
        check(leds(emu) == mask_to_leds(m), f'mask {m:#04x} -> {leds(emu)}')
    emu.systick(1000)                       # firmware keeps running its own (now dead) LED logic
    check(leds(emu) == mask_to_leds(0), 'still as commanded after 1 s more')
    outside = [c for c in callers if not (lo <= c[0] < hi)]
    check(callers and not outside,
          f'all {len(callers)} LED pin writes since reset come from DebugLedApply'
          + (f'; outside: {[(hex(a), hex(b)) for a, b in outside[:5]]}' if outside else ''))
    return check.done()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
