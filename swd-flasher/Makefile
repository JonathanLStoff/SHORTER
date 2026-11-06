# swd-flasher helper targets. Run from swd-flasher/.
#
# Bench tests of the slave board through the master's debug port (scripts/bench_slave.py):
#   make bench-slave            wheel test, then LED test
#   make bench-slave-wheel      slave wheel forward / stop / reverse at low speed (master wheel stays 0)
#   make bench-slave-leds       each slave LED on in turn, then all blink
#   make bench-slave-wheel SPEED=150 PORT=/dev/cu.usbmodem1101
#
# NOTE: the bench targets talk to the ESP32 console of the CURRENT swd-flasher firmware
# (CONTROL_MODE 1 in src/config.h, see API.md). They will need to be modified to work with the
# final version of the ESP32 firmware.
#
# Other targets:
#   make firmware               build firmware/master.bin and firmware/slave.bin
#   make test                   emulator tests of both images + ESP32 simulator test

PY    ?= .venv/bin/python
PORT  ?= /dev/cu.usbmodem101
SPEED ?= 100

.PHONY: bench-slave bench-slave-wheel bench-slave-leds firmware test

bench-slave:
	$(PY) scripts/bench_slave.py all --port $(PORT) --speed $(SPEED)

bench-slave-wheel:
	$(PY) scripts/bench_slave.py wheel --port $(PORT) --speed $(SPEED)

bench-slave-leds:
	$(PY) scripts/bench_slave.py leds --port $(PORT)

firmware:
	$(PY) scripts/build_hoverboard.py master
	$(PY) scripts/build_hoverboard.py slave

test:
	analysis/run_tests.sh
	$(MAKE) -C test test
