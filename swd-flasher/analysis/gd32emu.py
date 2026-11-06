"""Minimal GD32E230 emulator for checking firmware images on the host (no board needed).

Built on unicorn (Cortex-M, Thumb). Peripherals are plain RAM with a few behaviours:
  - RCU_CTL/CFG0 status bits mirror their enable/select bits (clock "ready" polls exit)
  - ADC_CTL1 calibration bits self-clear; ADC_STAT and USART_STAT report ready
  - GPIO ISTAT reads come from a hall-state model you can set (emu.hall = 0..7)
  - Flash-size register (0x1FFFF7E0) reads 64 KB
  - SysTick and TIMER0 update interrupts can be injected (run_isr) for virtual time
Every write to GPIO / TIMER0 / ADC / USART is logged with the PC that did it.

This is an approximation for register-level checks (pins, AF, timer setup, frames). It is not
cycle accurate and it does not model the real analog side.

Requires: pip install unicorn   (in swd-flasher/.venv)
"""
import collections
import os
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_CODE, UcError
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_XPSR,
                               UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                               UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12)

FLASH, SRAM, PER, PER_SZ = 0x08000000, 0x20000000, 0x40000000, 0x10000000
SRAM_SZ = 0x2000                 # GD32E230C8: 8 KB
SENTINEL = 0x08000F00           # return address for injected interrupts (never executed)
NM = os.environ.get('ARM_NM', '/Users/jonathanstoff/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm')

GPIO_BASES = {0x48000000: 'GPIOA', 0x48000400: 'GPIOB', 0x48000800: 'GPIOC', 0x48001400: 'GPIOF'}
GPIO_REGS = {0x00: 'CTL0', 0x04: 'OMODE', 0x08: 'OSPD', 0x0C: 'PUD', 0x10: 'ISTAT', 0x14: 'OCTL',
             0x18: 'BOP', 0x1C: 'LOCK', 0x20: 'AFSEL0', 0x24: 'AFSEL1', 0x28: 'BC', 0x2C: 'TG'}
TIMER_REGS = {0x00: 'CTL0', 0x04: 'CTL1', 0x08: 'SMCFG', 0x0C: 'DMAINTEN', 0x10: 'INTF', 0x14: 'SWEVG',
              0x18: 'CHCTL0', 0x1C: 'CHCTL1', 0x20: 'CHCTL2', 0x28: 'PSC', 0x2C: 'CAR', 0x30: 'CREP',
              0x34: 'CH0CV', 0x38: 'CH1CV', 0x3C: 'CH2CV', 0x40: 'CH3CV', 0x44: 'CCHP', 0x48: 'DMACFG', 0x4C: 'DMATB'}
USART_BASES = {0x40013800: 'USART0', 0x40004400: 'USART1'}
REG_ORDER = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
             UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
             UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_XPSR]


def symbols(elf):
    """Symbol table of an ELF file as {name: address}."""
    out = {}
    for line in subprocess.run([NM, '-n', elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def name_of(addr):
    for base, n in GPIO_BASES.items():
        if base <= addr < base + 0x400:
            return f'{n}.{GPIO_REGS.get(addr - base, hex(addr - base))}'
    if 0x40012C00 <= addr < 0x40013000:
        return f'TIMER0.{TIMER_REGS.get(addr - 0x40012C00, hex(addr - 0x40012C00))}'
    if 0x40012400 <= addr < 0x40012800:
        return f'ADC.{hex(addr - 0x40012400)}'
    for base, n in USART_BASES.items():
        if base <= addr < base + 0x400:
            return f'{n}.{hex(addr - base)}'
    if 0x40021000 <= addr < 0x40021400:
        return f'RCU.{hex(addr - 0x40021000)}'
    return f'{addr:#010x}'


class Emu:
    def __init__(self, image_path, elf=None):
        with open(image_path, 'rb') as f:
            self.fw = f.read()
        self.syms = symbols(elf) if elf else {}
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(FLASH, 0x10000)
        self.uc.mem_map(SRAM, SRAM_SZ * 4)
        self.uc.mem_map(PER, PER_SZ)
        self.uc.mem_map(0xE0000000, 0x100000)
        self.uc.mem_map(0x1FFFF000, 0x1000)
        self.uc.mem_write(0x1FFFF7E0, struct.pack('<H', 64))
        self.uc.mem_write(FLASH, self.fw)
        sp, reset = struct.unpack_from('<II', self.fw, 0)
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.reset_vector = reset & ~1
        self.vector_isr = {i: struct.unpack_from('<I', self.fw, i * 4)[0] & ~1 for i in range(48)}
        self.hall = 0b101           # hall state returned on ISTAT reads (bit2=A/PC14, bit1=B/PB11, bit0=C/PA1)
        self.log = []               # (pc, register name, size, value) for GPIO/TIMER/ADC/USART writes
        self.writes = collections.Counter()
        self.reads = collections.Counter()
        self._pc = self.reset_vector
        self.uc.hook_add(UC_HOOK_MEM_READ, self._on_read, None, PER, PER + PER_SZ - 1)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write, None, PER, PER + PER_SZ - 1)

    # ---- memory hooks ------------------------------------------------------------------------
    def _word(self, addr):
        return struct.unpack('<I', bytes(self.uc.mem_read(addr & ~3, 4)))[0]

    def _on_read(self, uc, access, addr, size, value, ud):
        cur = self._word(addr)
        v = cur
        if addr == 0x40021000:                     # RCU_CTL: ON bits -> stable bits
            for on, stb in ((0, 1), (16, 17), (24, 25)):
                if v >> on & 1:
                    v |= 1 << stb
        elif addr == 0x40021004:                   # RCU_CFG0: SCSS mirrors SCS
            v = (v & ~0xC) | ((v & 3) << 2)
        elif addr == 0x40012408:                   # ADC_CTL1: RSTCLB/CLB self-clear
            v &= ~0xC
        elif addr == 0x40012400:                   # ADC_STAT: EOC/EOIC
            v |= 0x6
        elif addr in (0x48000010, 0x48000410, 0x48000810):   # ISTAT: hall sensors
            a, b, c = (self.hall >> 2) & 1, (self.hall >> 1) & 1, self.hall & 1
            if addr == 0x48000010:  # PA: C on PA1
                v = (v & ~0x2) | (c << 1)
            elif addr == 0x48000410:  # PB: B on PB11
                v = (v & ~(1 << 11)) | (b << 11)
            else:                    # PC: A on PC14
                v = (v & ~(1 << 14)) | (a << 14)
        elif (addr & ~0x3FF) in USART_BASES and (addr & 0x3FF) == 0x1C:   # USART_STAT: TBE|TC|RBNE
            v |= (1 << 7) | (1 << 6) | (1 << 5)
        if v != cur:
            uc.mem_write(addr & ~3, struct.pack('<I', v))
        self.reads[name_of(addr)] += 1

    def _on_write(self, uc, access, addr, size, value, ud):
        n = name_of(addr)
        self.writes[n] += 1
        if n.startswith(('GPIO', 'TIMER', 'USART', 'ADC')):
            self.log.append((uc.reg_read(UC_ARM_REG_PC), n, size, value))

    # ---- execution ---------------------------------------------------------------------------
    def start(self):
        """Run from reset until the code has finished its initialisation (count instructions)."""
        self.run(300000)   # init finishes well inside this; the main loop is then waiting
        return self

    def run(self, count):
        try:
            self.uc.emu_start(self._pc | 1, 0, timeout=0, count=count)
        except UcError:
            pass
        self._pc = self.uc.reg_read(UC_ARM_REG_PC) & ~1

    def run_isr(self, addr, budget=5000):
        """Call a handler the way an exception would, return to the caller afterwards."""
        saved = [self.uc.reg_read(r) for r in REG_ORDER]
        pc = self.uc.reg_read(UC_ARM_REG_PC)
        self.uc.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
        try:
            self.uc.emu_start(addr | 1, SENTINEL, timeout=0, count=budget)
        except UcError:
            pass
        for r, v in zip(REG_ORDER, saved):
            self.uc.reg_write(r, v)
        self.uc.reg_write(UC_ARM_REG_PC, pc)

    def call(self, addr, budget=2000):
        """Call a function (AAPCS: return value in r0) and restore the registers afterwards."""
        saved = [self.uc.reg_read(r) for r in REG_ORDER]
        pc = self.uc.reg_read(UC_ARM_REG_PC)
        self.uc.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
        try:
            self.uc.emu_start(addr | 1, SENTINEL, timeout=0, count=budget)
        except UcError:
            pass
        r0 = self.uc.reg_read(UC_ARM_REG_R0)
        for r, v in zip(REG_ORDER, saved):
            self.uc.reg_write(r, v)
        self.uc.reg_write(UC_ARM_REG_PC, pc)
        return r0

    def set_physical_hall(self, hall_a, hall_b, hall_c):
        """Sensor levels on the board: A = PC14, B = PB11, C = PA1 (upstream names)."""
        self.hall = (hall_a << 2) | (hall_b << 1) | hall_c

    def gpio_levels(self):
        """Output levels replayed from the logged BOP/BC/OCTL writes: {'PB4': 1, ...}."""
        regs = {}
        levels = {}
        for _pc, n, _sz, v in self.log:
            if '.' not in n:
                continue
            port, reg = n.split('.', 1)
            if not port.startswith('GPIO'):
                continue
            p = port[4:]
            cur = levels.get(p, 0)
            if reg == 'BOP':
                cur = (cur | (v & 0xFFFF)) & ~((v >> 16) & 0xFFFF)
            elif reg == 'BC':
                cur &= ~(v & 0xFFFF)
            elif reg == 'OCTL':
                cur = v & 0xFFFF
            levels[p] = cur
        out = {}
        for p in 'ABCF':
            for pin in range(16):
                out[f'P{p}{pin}'] = (levels.get(p, 0) >> pin) & 1
        return out

    def systick(self, ms=1):
        """Virtual time: each call is one SysTick (msTicks++) when the image has SysTick_Handler."""
        handler = self.syms.get('SysTick_Handler', self.vector_isr[15])
        for _ in range(ms):
            self.run(2000)
            self.run_isr(handler)

    # ---- readers -----------------------------------------------------------------------------
    def rd32(self, addr):
        return self._word(addr)

    def wr32(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<I', value & 0xFFFFFFFF))

    def rd16(self, addr):
        return struct.unpack('<H', bytes(self.uc.mem_read(addr, 2)))[0]

    def gpio_state(self):
        """Per-pin configuration after init: {'PA8': ('AF2', pull), ...}. pull 0 none, 1 up, 2 down."""
        out = {}
        for base, port in ((0x48000000, 'A'), (0x48000400, 'B'), (0x48000800, 'C'), (0x48001400, 'F')):
            ctl, pud, af0, af1 = (self._word(base + o) for o in (0x00, 0x0C, 0x20, 0x24))
            for pin in range(16):
                mode = (ctl >> (2 * pin)) & 3
                af = ((af0 >> (4 * pin)) & 0xF) if pin < 8 else ((af1 >> (4 * (pin - 8))) & 0xF)
                label = ['IN', 'OUT', f'AF{af}', 'ANA'][mode]
                out[f'P{port}{pin}'] = (label, (pud >> (2 * pin)) & 3)
        return out

    def timer0(self):
        t = 0x40012C00
        names = ('CTL0', 'CTL1', 'PSC', 'CAR', 'CHCTL0', 'CHCTL1', 'CHCTL2', 'CCHP', 'CH0CV', 'CH1CV', 'CH2CV')
        offs = {'CTL0': 0x00, 'CTL1': 0x04, 'PSC': 0x28, 'CAR': 0x2C, 'CHCTL0': 0x18, 'CHCTL1': 0x1C,
                'CHCTL2': 0x20, 'CCHP': 0x44, 'CH0CV': 0x34, 'CH1CV': 0x38, 'CH2CV': 0x3C}
        return {n: self._word(t + offs[n]) for n in names}
