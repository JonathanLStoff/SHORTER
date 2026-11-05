// Board-level configuration. Edit this file, not the logic.
#pragma once

// ---- Targets (hoverboard motor controllers) --------------------------------
// How many motor controllers must be flashed before the LED turns green.
// 1 = one connector (GPIO1/0) that you move from board to board (the LED goes green per board,
// blue again when unplugged). 2 = boards A and B wired to separate pins at the same time.
#define NUM_TARGETS 1

// SWD pins per target (ESP32-C6 GPIO numbers). Set nrst to -1 if not wired.
// Avoid strapping pins (4, 5, 8, 9, 15), USB (12, 13) and the LED pin.
#define TARGET_A_NAME "A"
#define TARGET_A_SWCLK 1
#define TARGET_A_SWDIO 0
#define TARGET_A_NRST (-1)

#define TARGET_B_NAME "B"
#define TARGET_B_SWCLK 2
#define TARGET_B_SWDIO 3
#define TARGET_B_NRST (-1)

// SWD clock half period. 500 ns = ~1 MHz. Lengthen if your wires are long/noisy.
#define SWD_HALF_PERIOD_NS 500

// ESP32 pin drive strength for SWCLK/SWDIO: 0 (~5 mA, softest edges) .. 3 (~40 mA).
// Strong edges ring on hand-soldered wires and the GD32 sees extra clock edges
// (measured: 0/1 read DPIDR correctly, 2 = ESP default garbles it, 3 is worse).
#define SWD_DRIVE_STRENGTH 0

// Print a raw DPIDR bit-trace at several speeds/drive strengths on boot (troubleshooting).
#define SWD_LINK_DIAG 0

// Only write the embedded image to the board with this unique ID (as printed in the log, "uid=...").
// Comment out to flash any board. Known boards:
//   master 4337485409544c573453827e    slave 4337485416544c5734538631
#define FLASH_ONLY_UID "4337485416544c5734538631" // slave

// If the chip is read-protected (typical for hoverboards), erase it and remove the
// protection. This permanently wipes the original firmware.
#define ALLOW_UNLOCK 1

// Give up unlocking one board after this many tries (each try wears the option bytes).
#define MAX_UNLOCK_TRIES 3

// Read-only backup: when a controller is found and no image is embedded, print its whole
// flash over the serial log as "DUMP ..." lines (see scripts/capture_dump.py). Harmless; nothing is written.
#define DUMP_STOCK_FLASH 1

// ---- Status LED ------------------------------------------------------------
// WS2812 / NeoPixel on this GPIO (GPIO8 on the ESP32-C6-DevKitC-1 and most clones).
#define LED_GPIO 8
#define LED_BRIGHTNESS 40 // 0-255

// ---- Timing ----------------------------------------------------------------
#define RETRY_DELAY_MS 300
#define ERROR_HOLD_MS 2500 // how long red is shown before searching again
