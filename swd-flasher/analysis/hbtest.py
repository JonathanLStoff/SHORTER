"""Shared helpers for the emulator tests: checks, master-slave frames, motor-input recording."""
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0

MB = 0x20001800                 # master mailbox (overlay/Inc/remoteDebugPort.h)
MAGIC = 0x3258424D              # 'MBX2'
POWER_OFF_KEY = 0x2146464F      # 'OFF!'
OFF = dict(heartbeat=0x04, cmdMaster=0x08, cmdSlave=0x0C, enable=0x10, ledMaster=0x14, ledSlave=0x18,
           power=0x1C, echo=0x20, fwState=0x24, vBus=0x28, currentMaster=0x2C, currentSlave=0x30,
           speedMaster=0x34, speedSlave=0x38, odomMaster=0x3C, odomSlave=0x40, hallMaster=0x44,
           hallSlave=0x48, slaveRx=0x4C, uptimeMs=0x50)
FW_DRIVING, FW_STOPPED, FW_HB_SEEN, FW_SLAVE_LINK = 1, 2, 4, 8
WS_DISABLE, WS_SHUTOFF = 0x40, 0x80
LED_PINS = {'green': 'PB3', 'orange': 'PA15', 'red': 'PB4', 'upper': 'PF1', 'lower': 'PB5'}
LED_BITS = {'green': 1, 'orange': 2, 'red': 4, 'upper': 8, 'lower': 16}


class Checks:
    def __init__(self):
        self.failures = []

    def __call__(self, cond, text):
        print(('ok:   ' if cond else 'FAIL: ') + text)
        if not cond:
            self.failures.append(text)

    def done(self):
        print('\nALL PASSED' if not self.failures else f'\n{len(self.failures)} FAILED')
        return 1 if self.failures else 0


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def master_frame(pwm, wstate=0, bad=False):
    """SerialMaster2Slave: '/', int16 pwm, uint8 wState, uint16 CRC."""
    body = b'/' + struct.pack('<hB', pwm, wstate)
    return body + struct.pack('<H', crc16(body) ^ (1 if bad else 0))


def slave_reply(wstate, current, speed, odom):
    """SerialSlave2Master: '/', uint8 wState, float currentDC, float realSpeed, int32 iOdom, uint16 CRC."""
    body = b'/' + struct.pack('<Bffi', wstate, current, speed, odom)
    return body + struct.pack('<H', crc16(body))


def tx_bytes(emu, usart='USART1', start=0):
    """Bytes the firmware wrote to USARTx TDATA, from log index start on."""
    return bytes(v & 0xFF for _pc, n, _s, v in emu.log[start:] if n == f'{usart}.0x28')


def parse_frames(data, size, fmt):
    """Valid frames (CRC checked) of a given size in a byte stream: list of unpacked tuples."""
    out, i = [], 0
    while i + size <= len(data):
        if data[i] == 0x2F:
            chunk = data[i:i + size]
            if struct.unpack_from('<H', chunk, size - 2)[0] == crc16(chunk[:size - 2]):
                out.append(struct.unpack_from(fmt, chunk, 1))
                i += size
                continue
        i += 1
    return out


def master_frames(emu, start=0):
    """Frames the master sent to the slave: [(pwm, wState), ...]."""
    return parse_frames(tx_bytes(emu, 'USART1', start), 6, '<hB')


def slave_replies(emu, start=0):
    """Replies the slave sent to the master: [(wState, current, speed, odom), ...]."""
    return parse_frames(tx_bytes(emu, 'USART1', start), 16, '<Bffi')


def feed_uart(emu, data):
    """Feed bytes into the master-slave receive handler, one RX interrupt per byte."""
    rx = emu.syms['usart1_rx_buf']
    for b in data:
        emu.uc.mem_write(rx, bytes([b]))
        emu.run_isr(emu.syms['UpdateUSARTMasterSlaveInput'], 4000)


def record_motor_inputs(emu):
    """Every argument passed to SetBldcInput (int16 range), appended to the returned list."""
    inputs = []
    sbi = emu.syms['SetBldcInput'] & ~1

    def on_code(uc, addr, size, ud):
        if addr == sbi:
            v = uc.reg_read(UC_ARM_REG_R0) & 0xFFFFFFFF
            inputs.append(v - (1 << 32) if v & 0x80000000 else v)

    emu.uc.hook_add(UC_HOOK_CODE, on_code, None, sbi, sbi + 1)
    return inputs


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def f32(emu, addr):
    return struct.unpack('<f', bytes(emu.uc.mem_read(addr, 4)))[0]


def byte(emu, addr):
    return emu.uc.mem_read(addr, 1)[0]


def leds(emu):
    lv = emu.gpio_levels()
    return {k: lv[p] for k, p in LED_PINS.items()}


def mask_to_leds(mask):
    return {k: int(bool(mask & b)) for k, b in LED_BITS.items()}
