// Minimal bit-banged ARM SWD (Serial Wire Debug) master.
// Platform independent: all pin access goes through swd_io_t so the same code
// runs on the ESP32-C6 and in the host-side simulator test (test/).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*clk)(void *ctx, int level);        // drive SWCLK
    void (*dio_write)(void *ctx, int level);  // drive SWDIO level (only while output)
    void (*dio_mode)(void *ctx, int output);  // 1 = host drives SWDIO, 0 = released
    int (*dio_read)(void *ctx);               // sample SWDIO
    void (*nrst)(void *ctx, int level);       // optional (may be NULL): 0 = hold target in reset
    void (*delay)(void *ctx);                 // half clock period
    void (*delay_ms)(void *ctx, uint32_t ms);
} swd_io_t;

typedef struct {
    const swd_io_t *io;
    void *ctx;
    uint32_t csw;   // cached MEM-AP CSW value
    uint32_t dpidr; // filled by swd_connect()
    int last_ack;   // ACK bits (LSB first, 1 = OK, 7 = nobody driving) of the last DPIDR attempt
    uint32_t last_id; // raw value returned by the last DPIDR attempt
    uint32_t glitches; // read-buffer samples that disagreed (a measure of link noise)
} swd_t;

enum {
    SWD_OK = 0,
    SWD_ERR_NOTARGET = -1, // no ACK / line floating: nothing connected
    SWD_ERR_FAULT = -2,    // target answered FAULT
    SWD_ERR_WAIT = -3,     // WAIT retries exhausted
    SWD_ERR_PARITY = -4,
    SWD_ERR_TIMEOUT = -5,
    SWD_ERR_PROTO = -6,
};

const char *swd_strerror(int err);

void swd_init(swd_t *s, const swd_io_t *io, void *ctx);

// Line reset, JTAG->SWD switch, read DPIDR, power up debug domain.
int swd_connect(swd_t *s);

int swd_mem_read32(swd_t *s, uint32_t addr, uint32_t *v);
int swd_mem_write32(swd_t *s, uint32_t addr, uint32_t v);
int swd_mem_write16(swd_t *s, uint32_t addr, uint16_t v);
int swd_mem_write8(swd_t *s, uint32_t addr, uint8_t v);
// Read `words` 32-bit words (addr must be 4-aligned). Handles TAR wrap at 1 KB.
int swd_mem_read_block(swd_t *s, uint32_t addr, uint32_t *buf, size_t words);
// Returns SWD_OK if the DP has no sticky error flags set (and clears them if it has).
int swd_check_sticky(swd_t *s);
// Raw DP CTRL/STAT (sticky error bits: 1 ORUN, 4 CMP, 5 ERR, 7 WDATAERR).
int swd_read_ctrlstat(swd_t *s, uint32_t *v);

#ifdef __cplusplus
}
#endif
