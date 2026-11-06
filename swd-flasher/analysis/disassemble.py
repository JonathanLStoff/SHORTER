"""Linear-sweep disassembly of a GD32E230 image, with PC-relative literal values annotated.

usage: ../.venv/bin/python disassemble.py <image.bin> <out.dis>

The sweep starts after the vector table (0xC0) and skips undecodable halfwords as .hword. Literal
pool words loaded with `ldr rX, [pc, #imm]` are printed as `; =VALUE`, so peripheral bases such as
0x48000400 (GPIOB) or 0x40012c34 (TIMER0 CH0CV) are visible at the load site.
"""
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE = 0x08000000


def main(image, out):
    data = open(image, 'rb').read()
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    lines = [f'; image {image}, {len(data)} bytes, base {BASE:#010x}']
    for i in range(48):
        lines.append(f'; vec[{i}] = {struct.unpack_from("<I", data, i * 4)[0]:08x}')
    addr, end = 0xC0, min(len(data), 0xC800)
    while addr < end:
        ins = list(md.disasm(data[addr:addr + 4], BASE + addr, 1))
        if not ins:
            lines.append(f'{BASE + addr:08x}: {data[addr]:02x}{data[addr + 1]:02x}          .hword 0x{data[addr + 1] << 8 | data[addr]:04x}')
            addr += 2
            continue
        i = ins[0]
        text = f'{i.address:08x}: {i.bytes.hex():<12} {i.mnemonic:<8} {i.op_str}'
        if i.mnemonic == 'ldr' and '[pc' in i.op_str:
            imm = int(re.search(r'#(0x[0-9a-f]+|\d+)\]', i.op_str).group(1), 0)
            lit = ((i.address + 4) & ~3) + imm - BASE
            if 0 <= lit and lit + 4 <= len(data):
                text += f'    ; ={struct.unpack_from("<I", data, lit)[0]:08x}'
        lines.append(text)
        addr += i.size
    open(out, 'w').write('\n'.join(lines) + '\n')
    print(f'{len(lines)} lines -> {out}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
