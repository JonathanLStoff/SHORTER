"""Reset the ESP32-C6 and capture the 'DUMP' lines it prints into a binary file.

usage: capture_dump.py <serial-port> <out.bin> [--no-reset]
Verifies the CRC32 the ESP32 computed while reading, so a corrupted capture is rejected.
--no-reset: don't reset the ESP32, just listen (reset it yourself, e.g. RESET button).
Leave a few seconds between runs: rapid back-to-back resets once left the C6 hung until
RESET was held down.
"""
import binascii, re, struct, sys, time
import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port, out = args[0], args[1]
reset = "--no-reset" not in sys.argv

s = serial.Serial(port, 115200, timeout=0.5)
if reset:
    # DTR low first (DTR high during reset = "boot into download mode"), then pulse RTS = reset
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
data = {}
meta = ""
end_line = None
deadline = time.time() + 90
buf = b""
while time.time() < deadline and end_line is None:
    buf += s.read(65536)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        line = line.decode(errors="replace").strip()
        if "CPUID=" in line:
            print(line[line.index("CPUID="):])
        if line.startswith("DUMP block") or line.startswith("DUMP uid"):
            print(line)
        if line.startswith("DUMP BEGIN"):
            meta = line
        elif line.startswith("DUMP END"):
            end_line = line
            print(line)
        elif line.startswith("DUMP "):
            parts = line.split()
            if len(parts) == 34:
                data[int(parts[1], 16)] = b"".join(struct.pack("<I", int(w, 16)) for w in parts[2:])
if end_line is None:
    sys.exit("no 'DUMP END' seen: is the board powered and connected?")
if "OK" not in end_line:
    last = max(data) + 128 if data else 0
    sys.exit(f"ESP32 reported a read error after 0x{last:05x} bytes ({len(data)} lines): " + end_line + " | " + meta)
img = b"".join(data.get(o, b"\xff" * 128) for o in range(0, 0x10000, 128))
missing = [hex(o) for o in range(0, 0x10000, 128) if o not in data]
crc = binascii.crc32(img) & 0xFFFFFFFF
want = int(re.search(r"crc32=([0-9a-f]+)", end_line).group(1), 16)
print(meta)
print(f"captured {len(data)} lines, missing {len(missing)}, crc32 mine={crc:08x} esp32={want:08x}")
if missing or crc != want:
    sys.exit("capture incomplete or corrupted: not saved")
open(out, "wb").write(img)
print("saved", out, len(img), "bytes")
