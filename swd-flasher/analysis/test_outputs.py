"""Motor output protections (master and slave image): hardware break, dead time, output gate,
software current limit, invalid hall state.

usage: ../.venv/bin/python test_outputs.py <image.bin> <image.elf>
CalculateBLDC() (the 16 kHz motor routine) is called directly. TIMER0_CCHP bits:
POEN 15 (outputs on), OAEN 14 (hardware re-enables POEN at the next update), BRKP 13 (break
polarity, 0 = low), BRKEN 12 (break input on), DTCFG 7:0 (dead time).
"""
import struct
import sys

from gd32emu import Emu
from hbtest import Checks

CCHP = 0x40012C44
POEN, OAEN, BRKP, BRKEN = 1 << 15, 1 << 14, 1 << 13, 1 << 12
DEAD_TIME = 60
CONV = 0.201465201465        # MOTOR_AMP_CONV_DC_AMP: amps per ADC count
LIMIT = 2.5


def main(image, elf):
    emu = Emu(image, elf)
    check = Checks()
    s = emu.syms
    emu.systick(600)

    cchp = emu.rd32(CCHP)
    check(cchp & BRKEN and not cchp & BRKP, f'break input enabled, active low (CCHP {cchp:#06x}; stock 0xd020)')
    check(cchp & 0xFF == DEAD_TIME, f'dead time {cchp & 0xFF} ticks (stock 32; longer is safer)')
    pb12 = emu.gpio_state()['PB12']
    check(pb12 == ('AF2', 1), f'PB12 = TIMER0 BRKIN: {pb12} (stock: AF2, pull-up)')
    for pin in ('PA8', 'PA9', 'PA10', 'PB13', 'PB14', 'PB15'):
        st = emu.gpio_state()[pin]
        check(st[0] == 'AF2', f'{pin} gate output: {st[0]} (TIMER0)')

    emu.uc.mem_write(s['offsetcount'], struct.pack('<H', 1000))   # skip the ADC offset calibration

    def run(enable, hall, counts, outputs_on=True):
        """One motor routine pass. counts = simulated current in ADC counts above the offset."""
        emu.uc.mem_write(s['bldc_enable'], bytes([enable]))
        emu.uc.mem_write(s['offsetdc'], struct.pack('<h', -counts))   # adc reads 0 in the emulator
        emu.wr32(s['filter_reg'], 0)
        emu.wr32(s['bldc_outputFilterPwm'], 0)
        emu.wr32(CCHP, (emu.rd32(CCHP) | POEN | OAEN) if outputs_on else (emu.rd32(CCHP) & ~(POEN | OAEN)))
        emu.set_physical_hall(*hall)
        emu.call(s['CalculateBLDC'], 20000)
        return emu.rd32(CCHP)

    valid = (1, 0, 0)
    c = run(1, valid, 0, outputs_on=False)
    check(c & OAEN, 'enabled, valid hall, 0 A: outputs re-armed (OAEN set)')
    for hall in ((0, 0, 0), (1, 1, 1)):
        c = run(1, hall, 0)
        check(not c & POEN and not c & OAEN, f'invalid hall {hall}: POEN and OAEN cleared (all gates off)')
    c = run(0, valid, 0)
    check(not c & POEN, 'disabled: POEN cleared (upstream only cleared OAEN, which left the gates on)')
    under = int(LIMIT / CONV)          # 12 counts = 2.42 A
    over = under + 1                   # 13 counts = 2.62 A
    c = run(1, valid, under, outputs_on=False)
    check(c & OAEN, f'{under * CONV:.2f} A: below the limit, outputs on')
    c = run(1, valid, over)
    check(not c & POEN, f'{over * CONV:.2f} A: over the 2.5 A limit, outputs off')
    cur = struct.unpack('<f', bytes(emu.uc.mem_read(s['currentDC'], 4)))[0]
    check(abs(cur - over * CONV) < 1e-3, f'currentDC computed as {cur:.3f} A')

    return check.done()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
