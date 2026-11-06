#!/bin/sh
# Runs every emulator test against the current firmware/*.bin (build them first with
# scripts/build_hoverboard.py master|slave). Exit code 0 = all passed.
cd "$(dirname "$0")/.." || exit 1
PY=.venv/bin/python
M="firmware/master.bin .build-hoverboard/master/.pio/build/genericGD32E230C8/firmware.elf"
S="firmware/slave.bin .build-hoverboard/slave/.pio/build/genericGD32E230C8/firmware.elf"
fail=0
run() { name=$1; shift; out=$($PY "$@" 2>&1); res=$(echo "$out" | tail -1)
        printf '%-34s %s\n' "$name" "$res"; case $res in "ALL PASSED") ;; *) echo "$out" | grep FAIL; fail=1;; esac; }
run "master: mailbox + relay"   analysis/test_master_mailbox.py $M
run "slave: master frames"      analysis/test_slave_frames.py $S
run "master: LEDs"              analysis/test_leds.py $M
run "slave: LEDs"               analysis/test_leds.py $S
run "master: output protection" analysis/test_outputs.py $M
run "slave: output protection"  analysis/test_outputs.py $S
run "master: hall order vs stock" analysis/test_hall_order.py backup/master_stock.bin $M
run "slave: hall order vs stock"  analysis/test_hall_order.py backup/slave_stock.bin $S
exit $fail
