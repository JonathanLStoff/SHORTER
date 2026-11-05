"""Build the hoverboard (GD32E230) firmware for one board role, reproducibly.

usage: .venv/bin/python scripts/build_hoverboard.py slave [--layout 2]

Copies ../hoverboard-firmware-hack-GD32/HoverBoardGigaDevice to .build-hoverboard/<role>/
(the submodule itself is never modified), edits Inc/configDebug.h for the role, builds the
genericGD32E230C8 env (TARGET=3) with PlatformIO and writes firmware/<role>.bin.

Roles:
  slave  - SLAVE firmware: driven by the master over the original master-slave cable
           (USART1, PA2/PA3). Without a master sending commands its motor stays off.
"""
import argparse, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # swd-flasher/
SRC = os.path.join(HERE, "..", "hoverboard-firmware-hack-GD32", "HoverBoardGigaDevice")
PIO = os.path.join(HERE, ".venv", "bin", "pio")


def sub(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, flags=re.M)
    if n != 1:
        sys.exit(f"configDebug.h: expected exactly one match for {what}, found {n} (upstream changed?)")
    return new


def configure(cfg, role, layout):
    # Never ship autodetect: it spins the motor without hall feedback (bench supply only).
    cfg = sub(cfg, r"^#define REMOTE_AUTODETECT\b", "//#define REMOTE_AUTODETECT", "REMOTE_AUTODETECT")
    # Layout for the GD32E230 (TARGET 3): 2 = defines_2-3-2.h
    cfg = sub(cfg, r"(#elif GD32E230\s*//\s*TARGET = 3\s*\n\s*#define LAYOUT )\d+", rf"\g<1>{layout}", "E230 LAYOUT")
    if role == "slave":
        cfg = sub(cfg, r"^(\s*)//#define SLAVE\b", r"\1#define SLAVE", "SLAVE")
        cfg = sub(cfg, r"^(\s*)#define SINGLE\b", r"\1//#define SINGLE", "SINGLE")
    else:
        sys.exit(f"unknown role {role}")
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("role", choices=["slave"])
    ap.add_argument("--layout", type=int, default=2, help="E230 layout number (2 = 2.3.2)")
    a = ap.parse_args()

    build = os.path.join(HERE, ".build-hoverboard", a.role)
    if os.path.exists(build):
        shutil.rmtree(build)
    shutil.copytree(SRC, build, ignore=shutil.ignore_patterns(".pio"))
    path = os.path.join(build, "Inc", "configDebug.h")
    cfg = configure(open(path).read(), a.role, a.layout)
    open(path, "w").write(cfg)

    subprocess.run([PIO, "run", "-e", "genericGD32E230C8", "-d", build], check=True)
    out_bin = os.path.join(build, ".pio", "build", "genericGD32E230C8", "firmware.bin")
    dest = os.path.join(HERE, "firmware", f"{a.role}.bin")
    shutil.copyfile(out_bin, dest)
    rev = subprocess.run(["git", "-C", SRC, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"built {dest} ({os.path.getsize(dest)} bytes) from firmware commit {rev}, layout {a.layout}, role {a.role}")


if __name__ == "__main__":
    main()
