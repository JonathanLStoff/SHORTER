# Controller firmware images

The flasher embeds the image(s) here and writes them to the motor controllers. With the default
single connector (`NUM_TARGETS 1`) only `target_a.bin` is used, and it is written to **every** board
you connect, so put in the image for the board you are about to plug in, and remove it before
switching boards (see the top-level README, "Flashing a board"). To restore the stock firmware,
copy `../backup/master_stock.bin` (or `slave_stock.bin`) here as `target_a.bin`.

With `NUM_TARGETS 2` (each board on its own pins):

| File | Flashed to | Purpose |
|---|---|---|
| `target_a.bin` | board A (SWCLK=GPIO1, SWDIO=GPIO0) | `SLAVE_ID 0` |
| `target_b.bin` | board B (SWCLK=GPIO2, SWDIO=GPIO3) | `SLAVE_ID 1` |

If `target_b.bin` is missing, `target_a.bin` is used for both boards. The two boards then
share a `SLAVE_ID` and will both answer every command on the UART bus, so build two images.
With no images the flasher only reads a backup of each board it finds (see
`scripts/capture_dump.py`), turns green, and never writes anything.

After changing a `.bin`, run `pio run -t upload`; `scripts/gen_images.py` regenerates `src/fw_images.c` automatically.

## Building the images (old plan: ESP32 controls the boards over UART)

> Superseded: there are no spare holes for a UART link, so control will go over the debug port
> instead (custom RAM-mailbox mode, not written yet). The configuration steps below (layout,
> `REMOTE_AUTODETECT` off, `genericGD32E230C8`) still apply.

The GD32 firmware already supports an ESP32 acting as master (`REMOTE_UARTBUS`): 19200 baud on
the master-slave header (PA2/PA3), one frame per command
`{'/', type, slave id, speed, steer, state flags, CRC}`, and a reply with battery voltage,
current, speed and odometer. Motors stop if no command arrives for 500 ms. No firmware code
changes are needed, only configuration, in
`hoverboard-firmware-hack-GD32/HoverBoardGigaDevice/Inc/configDebug.h`:

1. Comment out `#define REMOTE_AUTODETECT` (line 4). That mode spins the motors to find the
   hall pins and is only meant for a current-limited bench supply.
2. In the `#else` branch set `LAYOUT` to the layout that matches **your PCB** (the version
   printed on the board, e.g. 2.1.x). A wrong layout drives the wrong pins. Do not guess.
3. Choose `#define SINGLE` (each board independent, which is what the ESP32 bus expects).
4. Comment out `#define REMOTE_DUMMY` and enable `#define REMOTE_UARTBUS`, with
   `#define SLAVE_ID 0` for `target_a.bin`, then `1` for `target_b.bin`.
5. Build `genericGD32E230C8` (`TARGET=3`) with PlatformIO in `HoverBoardGigaDevice/`
   and copy `.pio/build/genericGD32E230C8/firmware.bin` here. Build once per `SLAVE_ID`.

The E230 target is marked "not yet tested" upstream. Bench-test with a current-limited supply
(1-2 A) and the wheels off the ground.
