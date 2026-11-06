"""Build the hoverboard (GD32E230) firmware for one board role, reproducibly, and verify it.

usage: .venv/bin/python scripts/build_hoverboard.py {slave,master} [--layout 2]

Copies ../hoverboard-firmware-hack-GD32/HoverBoardGigaDevice to .build-hoverboard/<role>/ (the
submodule itself is never modified), applies the role settings, builds the genericGD32E230C8 env
with PlatformIO and writes firmware/<role>.bin plus firmware/<role>.build.txt.

Roles (full description: API.md):
  slave   SLAVE firmware. Takes its orders only from the master, over the master-slave cable
          (USART1, PA2/PA3): wheel command, disable, switch-off and its LEDs. Stops its wheel when
          no frame arrived for 800 ms. Never moves without a master frame.
  master  MASTER firmware. Its remote is the RAM mailbox (overlay/Src/remoteDebugPort.c) that the
          ESP32 writes over SWD: one command per wheel, LEDs of both boards, power off.
          It forwards the slave wheel and the slave LEDs to the slave over the cable.

Both images: no foot-pad/IMU code, halls used only for commutation (and reported), no battery,
inactivity or button power-off, no beeps except one short boot beep, hardware overcurrent break
(PB12) enabled, software current limit 2.5 A per board (24 V / 5 A supply).

The active configuration is checked after the build: the preprocessor must see the intended
role and layout, and the mailbox must sit in free RAM. The script refuses to write an image
that fails these checks.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # swd-flasher/
SRC = os.path.join(HERE, "..", "hoverboard-firmware-hack-GD32", "HoverBoardGigaDevice")
OVERLAY = os.path.join(HERE, "overlay")
PIO = os.path.join(HERE, ".venv", "bin", "pio")
TOOLCHAIN = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin")
ENV = "genericGD32E230C8"
DC_CUR_LIMIT_A = "2.5"   # amps per board (software current chopping in bldc.c)


def sub(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, flags=re.M)
    if n != 1:
        sys.exit(f"{what}: expected exactly one match, found {n} (upstream changed?)")
    return new


def enable_config_debug(cfg_select):
    # configSelect.h only includes configDebug.h when CONFIG_DEBUG is defined. Without it the build
    # silently uses config.h (SINGLE, REMOTE_DUMMY zigzag test, layout 1 = 2.3.1). That bug shipped before.
    return sub(cfg_select, r"^//#define CONFIG_DEBUG\b", "#define CONFIG_DEBUG", "CONFIG_DEBUG")


def configure(cfg, role, layout):
    # Never ship autodetect: it spins the motor without hall feedback (bench supply only).
    cfg = sub(cfg, r"^#define REMOTE_AUTODETECT\b", "//#define REMOTE_AUTODETECT", "REMOTE_AUTODETECT")
    # E230 (TARGET 3) layout: 2 = defines_2-3-2.h
    cfg = sub(cfg, r"(#elif GD32E230\s*//\s*TARGET = 3\s*\n\s*#define LAYOUT )\d+", rf"\g<1>{layout}",
              "E230 LAYOUT")
    # Only one remote may be active. SINGLE/MASTER pull in REMOTE_DUMMY (zigzag test), so turn it off.
    cfg = sub(cfg, r"^(\s*)#define REMOTE_DUMMY\s*$", r"\1//#define REMOTE_DUMMY", "REMOTE_DUMMY")
    # Nothing switches the board off or beeps on its own (bench supply, no battery)
    cfg = sub(cfg, r"^(\s*)#define BEEP_BACKWARDS\b", r"\1//#define BEEP_BACKWARDS", "BEEP_BACKWARDS")
    cfg = sub(cfg, r"^(\s*)#define INACTIVITY_TIMEOUT\b", r"\1//#define INACTIVITY_TIMEOUT", "INACTIVITY_TIMEOUT")
    # 24 V / 5 A converter feeds both boards: 2.5 A each keeps the sum under the supply limit
    cfg = sub(cfg, r"^#define DC_CUR_LIMIT(\s+)15\b", rf"#define DC_CUR_LIMIT\g<1>{DC_CUR_LIMIT_A}", "DC_CUR_LIMIT")
    if role == "slave":
        cfg = sub(cfg, r"^(\s*)//#define SLAVE\b", r"\1#define SLAVE", "SLAVE")
        cfg = sub(cfg, r"^(\s*)#define SINGLE\b", r"\1//#define SINGLE", "SINGLE")
    elif role == "master":
        cfg = sub(cfg, r"^(\s*)//#define MASTER\b", r"\1#define MASTER", "MASTER")
        cfg = sub(cfg, r"^(\s*)#define SINGLE\b", r"\1//#define SINGLE", "SINGLE")
        cfg = sub(cfg, r"^(\s*)//#define REMOTE_DEBUGPORT\b", r"\1#define REMOTE_DEBUGPORT", "REMOTE_DEBUGPORT") \
            if re.search(r"^\s*//#define REMOTE_DEBUGPORT\b", cfg, re.M) else \
            sub(cfg, r"^(\s*)//#define REMOTE_DUMMY\s*$", r"\1//#define REMOTE_DUMMY\n\1#define REMOTE_DEBUGPORT",
                "REMOTE_DEBUGPORT")
    else:
        sys.exit(f"unknown role {role}")
    return cfg


def patch_remote_h(text):
    # remote.h picks the remote header; add the mailbox remote in front of the dummy one
    return sub(text, r"^(\s*)#elif defined\(REMOTE_DUMMY\)",
               '\\1#elif defined(REMOTE_DEBUGPORT)\n\\1\t#include "../Inc/remoteDebugPort.h"\n\\1#elif defined(REMOTE_DUMMY)',
               "remote.h REMOTE_DUMMY branch")


def patch_leds(text):
    """Firmware LED writes go through LedWrite() so the debug port can take the LEDs over."""
    text, n1 = re.subn(r"digitalWrite\((LED_RED|LED_ORANGE|LED_GREEN|UPPER_LED|LOWER_LED)\b", r"LedWrite(\1", text)
    # ShowBatteryState() picks its LED pins through a table (aPin[] holds LED_GREEN/ORANGE/RED)
    text, n2 = re.subn(r"digitalWrite\(aPin\[", "LedWrite(aPin[", text)
    n = n1 + n2
    if n and '#include "../Inc/debugLed.h"' not in text:
        # after the first include of an upstream header, so the pin macros are already defined
        text, k = re.subn(r'^(#include "\.\./Inc/[^"]+"\s*)$', r'\1\n#include "../Inc/debugLed.h"', text, count=1, flags=re.M)
        if k != 1:
            sys.exit("LED redirect: no include to hook debugLed.h onto")
    return text


def patch_all_leds(build):
    """Redirect LED writes in every source file (IMU status code writes LED_RED too)."""
    src = os.path.join(build, "Src")
    touched = []
    for name in sorted(os.listdir(src)):
        if not name.endswith(".c") or name in ("debugLed.c", "remoteDebugPort.c", "slaveWatchdog.c"):
            continue
        path = os.path.join(src, name)
        with open(path) as f:
            text = f.read()
        new_text = patch_leds(text)
        if new_text != text:
            with open(path, "w") as f:
                f.write(new_text)
            touched.append(name)
    return touched


def patch_hall_order(text):
    """Hall bit order as the stock firmware reads it: state = PB11 + 2*PC14 + 4*PA1 (stock 0x08004DD2).
    Upstream's ReadHallState() is A + 2*B + 4*C, so A and B must be swapped for these pins to give the
    same state as the stock code. Verified by analysis/test_hall_order.py."""
    text = sub(text, r"^#define HALL_A(\s+)PC14\b", r"#define HALL_A\1PB11", "HALL_A")
    return sub(text, r"^#define HALL_B(\s+)PB11\b", r"#define HALL_B\1PC14", "HALL_B")


def patch_comms(text):
    text = sub(text, r'^#include "../Inc/commsMasterSlave.h"\s*$',
               '#include "../Inc/commsMasterSlave.h"\n#include "../Inc/slaveWatchdog.h"\n#include "../Inc/remoteDebugPort.h"',
               "comms include")
    # slave: every valid master frame feeds the slave watchdog (upstream ResetTimeout does nothing on E230)
    text = sub(text, r"^(\s*)ResetTimeout\(\);[ \t]*$", r"\1ResetTimeout();\n\1SlaveFrameSeen();", "comms ResetTimeout")
    # master: count slave replies for the debug port (link status)
    text = sub(text, r"^(\s*)oDataSlave\.iOdom\s*= pData->iOdom;[ \t]*$",
               r"\g<0>\n\1DebugPortSlaveReply();", "comms slave reply")
    # slave reply: hall state in wState bits 4..6, bit 7 = valid (bits 0..3 keep the upstream meaning)
    return sub(text, r"^(\s*)oData\.wState(\s*)= sendByte;[ \t]*$",
               r"\1{ extern volatile uint8_t hall; oData.wState = (uint8_t)(sendByte | 0x80u | ((hall & 7u) << 4)); }",
               "comms slave hall")


def patch_main(text):
    text = sub(text, r'^#include "../Inc/commsMasterSlave.h"\s*$',
               '#include "../Inc/commsMasterSlave.h"\n#include "../Inc/slaveWatchdog.h"', "main include")
    text = sub(text, r"^(\s*)SetBldcInput\(pwmSlave\);", r"\1SlaveWatchdogUpdate();\n\1SetBldcInput(pwmSlave);",
               "main SetBldcInput(pwmSlave)")
    # master: one command per wheel from the mailbox instead of the speed/steer mix
    text = sub(text, r"^(\s*)#ifdef PILOT_DEFAULT[ \t]*\n(?=\s*if \(iDrivingMode == 0\))",
               "\\1#if defined(REMOTE_DEBUGPORT)\t// debug port: one command per wheel, no mixing\n"
               "\\1\tpwmMaster = DebugPortPwmMaster();\n\\1\tpwmSlave = DebugPortPwmSlave();\n"
               "\\1#elif defined(PILOT_DEFAULT)\n", "main mix bypass")
    # upstream bug: `& !STATE_Disable` is `& 0` and wiped every bit (LEDs) sent to the slave
    text = sub(text, r"wStateSlave = wStateSlave & !STATE_Disable;", "wStateSlave = wStateSlave & ~STATE_Disable;",
               "main wStateSlave disable bit")
    # boot: one short beep instead of the 0.9 s melody
    text = sub(text, r"^(\s*)BUZZER_MelodyDown\(\)[ \t]*$", r"\1BuzzerSet(4,0) Delay(150); BuzzerSet(0,0)", "boot melody")
    # switch-off: no melody (it delayed the shutdown by 0.9 s)
    return sub(text, r"^(\s*)BUZZER_MelodyUp\(\)[ \t]*$", r"\1// shutdown melody removed", "shutdown melody")


def patch_layout(text):
    """Layout 2.3.2 changes: hall order as stock, no button logic, stock overcurrent break input."""
    text = patch_hall_order(text)
    # The power button only boots the board (hardware latch). The firmware never reads it, so it
    # cannot switch the board off. Shutdown is the debug-port `off` command.
    text = sub(text, r"^#define BUTTON_PU(\s+)PA5\b", r"//#define BUTTON_PU\1PA5\t// removed: button ignored after boot", "BUTTON_PU")
    # Stock firmware enables the TIMER0 break input (CCHP = 0xD020: BRKEN, active low) with PB12 as AF2,
    # pull-up. Upstream leaves it off for this layout.
    text = sub(text, r"^(#define SELF_HOLD\s+PB2\b.*)$", r"\1\n#define TIMER_BLDC_EMERGENCY_SHUTDOWN\tPB12\t// TIMER0 BRKIN, overcurrent (stock)", "SELF_HOLD line")
    return text


def patch_setup(text):
    # break input: pull-up like stock (idle high, the overcurrent signal pulls it low)
    text = sub(text, r"pinModeAF\(TIMER_BLDC_EMERGENCY_SHUTDOWN,AF_TIMER0_BRKIN,GPIO_PUPD_NONE,",
               "pinModeAF(TIMER_BLDC_EMERGENCY_SHUTDOWN,AF_TIMER0_BRKIN,GPIO_PUPD_PULLUP,", "break pin pull")
    # PWM_init(): enable the break (the second, commented line stays as it is)
    return sub(text, r"^(\s*timerBldc_break_parameter_struct\.breakstate\s*=\s*)TIMER_BREAK_DISABLE;(\s*// Gen2\.2 HarleyBob)",
               r"\1TIMER_BREAK_ENABLE;\2", "PWM_init breakstate")


def patch_bldc(text):
    """Turning the outputs off must clear POEN, not only OAEN (see overlay/Inc/bldcOutputs.h)."""
    text = sub(text, r'^(#include "\.\./Inc/bldc\.h"[ \t]*)$', r'\1\n#include "../Inc/bldcOutputs.h"', "bldc include")
    text, n_off = re.subn(r"timer_automatic_output_disable\(TIMER_BLDC\);", "BldcOutputsOff();", text)
    text, n_on = re.subn(r"timer_automatic_output_enable\(TIMER_BLDC\);", "BldcOutputsOn();", text)
    if (n_off, n_on) != (2, 1):
        sys.exit(f"bldc output gate: expected 2 disable / 1 enable calls, found {n_off} / {n_on}")
    return text


def preprocess_macros(build, layout=None):
    """Return the macros the compiler sees from the configuration headers (not from our regexes).
    With layout, the board pin file is included too (it is plain #defines)."""
    probe = os.path.join(build, ".probe_config.c")
    with open(probe, "w") as f:
        f.write('#include "Inc/configSelect.h"\n')
        if layout is not None:
            f.write(f'#include "Inc/defines/defines_2-3-{layout}.h"\n')
    try:
        out = subprocess.run([os.path.join(TOOLCHAIN, "arm-none-eabi-gcc"), "-E", "-dM", "-DGD32E230=1",
                              "-DTARGET=3", "-I.", probe], cwd=build, capture_output=True, text=True, check=True).stdout
    finally:
        os.remove(probe)
    macros = {}
    for line in out.splitlines():
        m = re.match(r"#define (\w+)(?:\s+(.*))?$", line)
        if m:
            macros[m.group(1)] = (m.group(2) or "1").strip()
    return macros


def check_mailbox_contract():
    """The ESP32 (src/mailbox.h) and the firmware (overlay/Inc/remoteDebugPort.h) must agree."""
    def grab(path, name):
        with open(path) as f:
            hit = re.search(rf"#define {name}\s+(0x[0-9A-Fa-f]+)", f.read())
        if not hit:
            sys.exit(f"mailbox contract: {name} not found in {path}")
        return int(hit.group(1), 16)
    esp = os.path.join(HERE, "src", "mailbox.h")
    fw = os.path.join(OVERLAY, "Inc", "remoteDebugPort.h")
    for name_esp, name_fw in (("MBX_ADDR", "MAILBOX_ADDR"), ("MBX_MAGIC", "MAILBOX_MAGIC"),
                              ("MBX_BYTES", "MAILBOX_SIZE")):
        a, b = grab(esp, name_esp), grab(fw, name_fw)
        if a != b:
            sys.exit(f"mailbox contract: {name_esp}={a:#x} in src/mailbox.h but {name_fw}={b:#x} in the firmware")
    return grab(fw, "MAILBOX_SIZE")


def verify(build, role, layout):
    global MAILBOX_BYTES
    MAILBOX_BYTES = check_mailbox_contract()
    m = preprocess_macros(build)
    want = {
        "CONFIG_DEBUG": True,
        "LAYOUT": str(layout),
        "MASTERSLAVE_USART": "1",
        "BLDC_SINE": True,
        "SINGLE": False,
        "REMOTE_DUMMY": False,
        "REMOTE_AUTODETECT": False,
    }
    # hall-driven test features must stay off: they would drive the LEDs or spin the wheel by themselves
    for off in ("TEST_HALL2LED", "DEBUG_LED", "REMOTE_AUTODETECT", "RTT_REMOTE", "REMOTE_UARTBUS", "REMOTE_UART"):
        want[off] = False
    # the foot-pad/IMU balance code must not be built in: only the debug port moves the wheels
    for off in ("MPU_6050", "MPU_6050old", "BMI_160", "SEND_IMU_DATA", "PILOT_HOVERBIKE", "PILOT_USER"):
        want[off] = False
    # nothing switches the board off or beeps on its own
    for off in ("BEEP_BACKWARDS", "INACTIVITY_TIMEOUT", "BATTERY_LOW_SHUTOFF", "BATTERY_LOW_BEEP"):
        want[off] = False
    want["DC_CUR_LIMIT"] = DC_CUR_LIMIT_A
    if role == "slave":
        want.update({"SLAVE": True, "MASTER": False, "REMOTE_DEBUGPORT": False})
    else:
        want.update({"MASTER": True, "SLAVE": False, "REMOTE_DEBUGPORT": True})
    errors = []
    for k, v in want.items():
        present = k in m
        if isinstance(v, bool):
            if present != v:
                errors.append(f"{k}: expected {'defined' if v else 'undefined'}")
        elif not present or m[k] != v:
            errors.append(f"{k}: expected {v}, compiler sees {m.get(k, 'undefined')}")
    # board pins (layout file): hall order, no button, break input, power latch
    lm = preprocess_macros(build, layout)
    pins = {"HALL_A": "PB11", "HALL_B": "PC14", "HALL_C": "PA1", "SELF_HOLD": "PB2",
            "TIMER_BLDC_EMERGENCY_SHUTDOWN": "PB12", "LED_GREEN": "PB3", "LED_ORANGE": "PA15",
            "LED_RED": "PB4", "UPPER_LED": "PF1", "LOWER_LED": "PB5",
            "BLDC_YH": "PA8", "BLDC_YL": "PB13", "BLDC_BH": "PA9", "BLDC_BL": "PB14",
            "BLDC_GH": "PA10", "BLDC_GL": "PB15"}
    for k, v in pins.items():
        got = lm.get(k, "undefined").split("//")[0].strip()
        if got != v:
            errors.append(f"{k}: expected {v}, compiler sees {got}")
    for off in ("BUTTON", "BUTTON_PU"):
        if off in lm:
            errors.append(f"{off}: expected undefined (button is ignored after boot)")
    if errors:
        sys.exit("configuration check failed:\n  " + "\n  ".join(errors))
    return m


MAILBOX_BYTES = 0x54   # must equal MAILBOX_SIZE (checked by check_mailbox_contract)


def check_memory(elf, role):
    """The mailbox must not overlap .bss or the top of RAM kept for the stack."""
    from_nm = subprocess.run([os.path.join(TOOLCHAIN, "arm-none-eabi-nm"), "-n", elf],
                             capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in from_nm.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    mailbox = 0x20001800
    size = MAILBOX_BYTES
    ebss = syms["_ebss"]
    estack = syms["_estack"]
    min_stack = syms.get("_Min_Stack_Size", 0x400)
    if not (ebss + 0x100 <= mailbox):
        sys.exit(f"{role}: mailbox {mailbox:#x} overlaps .bss (ends {ebss:#x})")
    if not (mailbox + size <= estack - min_stack):
        sys.exit(f"{role}: mailbox overlaps the stack reserve (stack top {estack:#x})")
    return {"mailbox": mailbox, "bss_end": ebss, "stack_top": estack}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("role", choices=["slave", "master"])
    ap.add_argument("--layout", type=int, default=2, help="E230 layout number (2 = 2.3.2)")
    a = ap.parse_args()

    build = os.path.join(HERE, ".build-hoverboard", a.role)
    if os.path.exists(build):
        shutil.rmtree(build)
    shutil.copytree(SRC, build, ignore=shutil.ignore_patterns(".pio"))

    # overlay: new source files, and the two header edits that let remote.h pick the mailbox remote
    for sub_dir in ("Src", "Inc"):
        for name in os.listdir(os.path.join(OVERLAY, sub_dir)):
            shutil.copyfile(os.path.join(OVERLAY, sub_dir, name), os.path.join(build, sub_dir, name))
    def edit(path, fn):
        with open(path) as f:
            text = f.read()          # read first: opening for write would truncate the file
        with open(path, "w") as f:
            f.write(fn(text))

    edit(os.path.join(build, "Inc", "remote.h"), patch_remote_h)
    edit(os.path.join(build, "Inc", "configSelect.h"), enable_config_debug)
    edit(os.path.join(build, "Inc", "configDebug.h"), lambda t: configure(t, a.role, a.layout))
    edit(os.path.join(build, "Src", "commsMasterSlave.c"), patch_comms)
    edit(os.path.join(build, "Src", "main.c"), patch_main)
    edit(os.path.join(build, "Src", "setup.c"), patch_setup)
    edit(os.path.join(build, "Src", "bldc.c"), patch_bldc)
    patch_all_leds(build)
    edit(os.path.join(build, "Inc", "defines", "defines_2-3-2.h"), patch_layout)

    verify(build, a.role, a.layout)

    subprocess.run([PIO, "run", "-e", ENV, "-d", build], check=True)
    elf = os.path.join(build, ".pio", "build", ENV, "firmware.elf")
    out_bin = os.path.join(build, ".pio", "build", ENV, "firmware.bin")
    mem = check_memory(elf, a.role)

    dest = os.path.join(HERE, "firmware", f"{a.role}.bin")
    shutil.copyfile(out_bin, dest)
    data = open(dest, "rb").read()
    rev = subprocess.run(["git", "-C", SRC, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    sha = hashlib.sha256(data).hexdigest()
    info = [
        f"role: {a.role}",
        f"layout: E230 {a.layout}",
        f"firmware commit: {rev}",
        f"sha256: {sha}",
        f"size: {len(data)}",
        (f"mailbox: {mem['mailbox']:#010x} ({MAILBOX_BYTES:#x} bytes, magic MBX2)" if a.role == "master"
         else "mailbox: none (slave takes orders from the master frame only)")
        + f"  bss_end: {mem['bss_end']:#010x}  stack_top: {mem['stack_top']:#010x}",
        f"current limit: {DC_CUR_LIMIT_A} A   heartbeat timeout: 600 ms (master)   frame timeout: 800 ms (slave)",
    ]
    open(os.path.join(HERE, "firmware", f"{a.role}.build.txt"), "w").write("\n".join(info) + "\n")
    print(f"built {dest} ({len(data)} bytes)\n  " + "\n  ".join(info))


if __name__ == "__main__":
    main()
