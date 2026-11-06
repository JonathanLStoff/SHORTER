#include "swd.h"

#include <string.h>

// DP register addresses (A[3:2] << 2)
#define DP_DPIDR   0x0 // read
#define DP_ABORT   0x0 // write
#define DP_CTRL    0x4
#define DP_SELECT  0x8
#define DP_RDBUFF  0xC
// AP (MEM-AP) register addresses, all in bank 0 (SELECT = 0)
#define AP_CSW     0x0
#define AP_TAR     0x4
#define AP_DRW     0xC

#define CTRL_CDBGPWRUPREQ (1u << 28)
#define CTRL_CDBGPWRUPACK (1u << 29)
#define CTRL_CSYSPWRUPREQ (1u << 30)
#define CTRL_CSYSPWRUPACK (1u << 31)
#define CTRL_STICKYORUN   (1u << 1)
#define CTRL_STICKYCMP    (1u << 4)
#define CTRL_STICKYERR    (1u << 5)
#define CTRL_WDATAERR     (1u << 7)

#define ABORT_CLEAR_ALL 0x1E // STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR

// CSW: DbgSwEnable + HPROT/master bits (0x23000000) | size | auto-increment
#define CSW_BASE   0x23000000u
#define CSW_SIZE8  0x0u
#define CSW_SIZE16 0x1u
#define CSW_SIZE32 0x2u
#define CSW_INC    0x10u

#define ACK_OK    1
#define ACK_WAIT  2
#define ACK_FAULT 4

#define WAIT_RETRIES 200

const char *swd_strerror(int err)
{
    switch (err) {
    case SWD_OK: return "ok";
    case SWD_ERR_NOTARGET: return "no target answering on SWD";
    case SWD_ERR_FAULT: return "target returned FAULT";
    case SWD_ERR_WAIT: return "target stuck returning WAIT";
    case SWD_ERR_PARITY: return "parity error";
    case SWD_ERR_TIMEOUT: return "timeout";
    default: return "protocol error";
    }
}

// ---------------------------------------------------------------- bit level

static void bit_out(swd_t *s, int bit)
{
    const swd_io_t *io = s->io;
    io->clk(s->ctx, 0);
    io->dio_write(s->ctx, bit);
    io->delay(s->ctx);
    io->clk(s->ctx, 1); // target samples on the rising edge
    io->delay(s->ctx);
}

static int bit_in(swd_t *s)
{
    const swd_io_t *io = s->io;
    io->clk(s->ctx, 0);
    io->delay(s->ctx);
    int b = io->dio_read(s->ctx); // target drives after the rising edge, valid by now
    io->clk(s->ctx, 1);
    io->delay(s->ctx);
    return b;
}

static void bits_out(swd_t *s, uint32_t v, int n) // LSB first
{
    for (int i = 0; i < n; i++)
        bit_out(s, (v >> i) & 1);
}

static void turnaround(swd_t *s) // one clock with SWDIO released
{
    (void)bit_in(s);
}

static int parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

static void line_reset(swd_t *s) // >= 50 clocks with SWDIO high
{
    s->io->dio_mode(s->ctx, 1);
    for (int i = 0; i < 56; i++)
        bit_out(s, 1);
}

static void idle(swd_t *s, int n)
{
    s->io->dio_mode(s->ctx, 1);
    for (int i = 0; i < n; i++)
        bit_out(s, 0);
}

// ----------------------------------------------------------- transfer level

// One SWD transfer. apndp: 0 = DP, 1 = AP. addr: byte address of the register
// (only bits 3:2 matter). For reads *data receives the value, for writes *data is sent.
static int xfer_once(swd_t *s, int apndp, int rnw, uint32_t addr, uint32_t *data, int *ack_out)
{
    int a2 = (addr >> 2) & 1, a3 = (addr >> 3) & 1;
    int par = (apndp + rnw + a2 + a3) & 1;
    uint32_t req = 1u | (apndp << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (par << 5) | (0u << 6) | (1u << 7);

    s->io->dio_mode(s->ctx, 1);
    bits_out(s, req, 8);
    s->io->dio_mode(s->ctx, 0);
    turnaround(s);

    int ack = bit_in(s);
    ack |= bit_in(s) << 1;
    ack |= bit_in(s) << 2;
    *ack_out = ack;
    s->last_ack = ack;

    if (ack != ACK_OK) {
        turnaround(s);
        idle(s, 8);
        return ack == ACK_WAIT ? SWD_ERR_WAIT : (ack == ACK_FAULT ? SWD_ERR_FAULT : SWD_ERR_NOTARGET);
    }

    if (rnw) {
        uint32_t v = 0;
        for (int i = 0; i < 32; i++)
            v |= (uint32_t)bit_in(s) << i;
        int p = bit_in(s);
        turnaround(s);
        idle(s, 8);
        if (p != parity32(v))
            return SWD_ERR_PARITY;
        *data = v;
        return SWD_OK;
    }

    turnaround(s);
    s->io->dio_mode(s->ctx, 1);
    bits_out(s, *data, 32);
    bit_out(s, parity32(*data));
    idle(s, 8);
    return SWD_OK;
}

static int xfer(swd_t *s, int apndp, int rnw, uint32_t addr, uint32_t *data)
{
    int ack = 0, rc = SWD_ERR_WAIT;
    for (int i = 0; i < WAIT_RETRIES; i++) {
        rc = xfer_once(s, apndp, rnw, addr, data, &ack);
        if (rc != SWD_ERR_WAIT)
            break;
    }
    if (rc == SWD_ERR_FAULT) { // clear sticky flags so the next transfer can work
        uint32_t abort = ABORT_CLEAR_ALL;
        int a;
        (void)xfer_once(s, 0, 0, DP_ABORT, &abort, &a);
    }
    return rc;
}

static int dp_read(swd_t *s, uint32_t reg, uint32_t *v) { return xfer(s, 0, 1, reg, v); }
static int dp_write(swd_t *s, uint32_t reg, uint32_t v) { return xfer(s, 0, 0, reg, &v); }
static int ap_write(swd_t *s, uint32_t reg, uint32_t v) { return xfer(s, 1, 0, reg, &v); }

// RDBUFF returns the result of the last AP read. On the GD32E230 only the FIRST read after an
// AP read is valid: a repeated RDBUFF read returns 0. So read it exactly once; integrity is
// checked one level up (block reads are done twice and compared).
static int rdbuff_read(swd_t *s, uint32_t *v) { return dp_read(s, DP_RDBUFF, v); }

// AP reads are posted: issue the read, then collect the result from RDBUFF.
static int ap_read(swd_t *s, uint32_t reg, uint32_t *v)
{
    uint32_t dummy;
    int rc = xfer(s, 1, 1, reg, &dummy);
    if (rc)
        return rc;
    return rdbuff_read(s, v);
}

// ------------------------------------------------------------------ connect

void swd_init(swd_t *s, const swd_io_t *io, void *ctx)
{
    s->io = io;
    s->ctx = ctx;
    s->csw = 0xFFFFFFFFu;
    s->dpidr = 0;
    s->last_ack = -1;
    s->last_id = 0;
    s->glitches = 0;
}

int swd_connect(swd_t *s)
{
    int rc = SWD_ERR_NOTARGET;
    uint32_t id = 0;

    s->csw = 0xFFFFFFFFu;
    for (int attempt = 0; attempt < 3; attempt++) {
        line_reset(s);
        bits_out(s, 0xE79E, 16); // JTAG -> SWD select sequence
        line_reset(s);
        idle(s, 8);

        int ack;
        id = 0;
        rc = xfer_once(s, 0, 1, DP_DPIDR, &id, &ack);
        s->last_ack = ack;
        s->last_id = id;
        if (rc == SWD_OK && id != 0 && id != 0xFFFFFFFFu && (id & 1))
            break;
        if (rc == SWD_OK)
            rc = SWD_ERR_NOTARGET; // garbage IDCODE: floating line
        s->io->delay_ms(s->ctx, 5);
    }
    if (rc)
        return rc;
    s->dpidr = id;

    uint32_t v;
    if ((rc = dp_write(s, DP_ABORT, ABORT_CLEAR_ALL)))
        return rc;
    if ((rc = dp_write(s, DP_SELECT, 0)))
        return rc;
    if ((rc = dp_write(s, DP_CTRL, CTRL_CDBGPWRUPREQ | CTRL_CSYSPWRUPREQ)))
        return rc;
    for (int i = 0; i < 100; i++) {
        if ((rc = dp_read(s, DP_CTRL, &v)))
            return rc;
        if ((v & (CTRL_CDBGPWRUPACK | CTRL_CSYSPWRUPACK)) == (CTRL_CDBGPWRUPACK | CTRL_CSYSPWRUPACK))
            return SWD_OK;
        s->io->delay_ms(s->ctx, 1);
    }
    return SWD_ERR_TIMEOUT;
}

int swd_read_ctrlstat(swd_t *s, uint32_t *v) { return dp_read(s, DP_CTRL, v); }

int swd_check_sticky(swd_t *s)
{
    uint32_t v;
    int rc = dp_read(s, DP_CTRL, &v);
    if (rc)
        return rc;
    if (v & (CTRL_STICKYERR | CTRL_STICKYCMP | CTRL_STICKYORUN | CTRL_WDATAERR)) {
        dp_write(s, DP_ABORT, ABORT_CLEAR_ALL);
        return SWD_ERR_FAULT;
    }
    return SWD_OK;
}

// ------------------------------------------------------------- memory access

static int set_csw(swd_t *s, uint32_t size_inc)
{
    uint32_t want = CSW_BASE | size_inc;
    if (s->csw == want)
        return SWD_OK;
    int rc = ap_write(s, AP_CSW, want);
    if (!rc)
        s->csw = want;
    return rc;
}

static int swd_mem_read32_once(swd_t *s, uint32_t addr, uint32_t *v)
{
    int rc;
    if ((rc = set_csw(s, CSW_SIZE32)))
        return rc;
    if ((rc = ap_write(s, AP_TAR, addr)))
        return rc;
    return ap_read(s, AP_DRW, v);
}

static int swd_mem_write32_once(swd_t *s, uint32_t addr, uint32_t v)
{
    int rc;
    if ((rc = set_csw(s, CSW_SIZE32)))
        return rc;
    if ((rc = ap_write(s, AP_TAR, addr)))
        return rc;
    return ap_write(s, AP_DRW, v);
}

static int swd_mem_write16_once(swd_t *s, uint32_t addr, uint16_t v)
{
    int rc;
    if ((rc = set_csw(s, CSW_SIZE16)))
        return rc;
    if ((rc = ap_write(s, AP_TAR, addr)))
        return rc;
    // Data travels on the byte lanes selected by addr[1:0]
    return ap_write(s, AP_DRW, (uint32_t)v << ((addr & 2) * 8));
}

static int swd_mem_write8_once(swd_t *s, uint32_t addr, uint8_t v)
{
    int rc;
    if ((rc = set_csw(s, CSW_SIZE8)))
        return rc;
    if ((rc = ap_write(s, AP_TAR, addr)))
        return rc;
    // Data travels on the byte lane selected by addr[1:0]
    return ap_write(s, AP_DRW, (uint32_t)v << ((addr & 3) * 8));
}

static int swd_mem_read_block_once(swd_t *s, uint32_t addr, uint32_t *buf, size_t words)
{
    int rc;
    if ((rc = set_csw(s, CSW_SIZE32 | CSW_INC)))
        return rc;
    size_t done = 0;
    while (done < words) {
        // TAR auto-increment is only guaranteed within a 1 KB block
        size_t chunk = (0x400 - ((addr + done * 4) & 0x3FF)) / 4;
        if (chunk > words - done)
            chunk = words - done;
        if ((rc = ap_write(s, AP_TAR, addr + done * 4)))
            return rc;
        uint32_t prev;
        // Pipelined: read i returns data of read i-1. Issue chunk reads, then RDBUFF.
        for (size_t i = 0; i < chunk; i++) {
            if ((rc = xfer(s, 1, 1, AP_DRW, &prev)))
                return rc;
            if (i > 0)
                buf[done + i - 1] = prev;
        }
        if ((rc = rdbuff_read(s, &prev)))
            return rc;
        buf[done + chunk - 1] = prev;
        done += chunk;
    }
    return SWD_OK;
}

// ----------------------------------------------------- recovery from line glitches
// A garbled ACK / parity error on a noisy wire desynchronises the protocol. Resynchronise
// (line reset + reconnect) and retry the operation a few times before giving up.
#define OP_RETRIES 4

static int recoverable(int rc)
{
    return rc == SWD_ERR_NOTARGET || rc == SWD_ERR_PARITY || rc == SWD_ERR_PROTO || rc == SWD_ERR_TIMEOUT;
}

#define RETRY_OP(call)                                   \
    do {                                                 \
        int _rc = (call);                                \
        for (int _i = 0; recoverable(_rc) && _i < OP_RETRIES; _i++) { \
            s->io->delay_ms(s->ctx, 2);                  \
            if (swd_connect(s) != SWD_OK)                \
                continue;                                \
            _rc = (call);                                \
        }                                                \
        return _rc;                                      \
    } while (0)

int swd_mem_read32(swd_t *s, uint32_t addr, uint32_t *v) { RETRY_OP(swd_mem_read32_once(s, addr, v)); }
int swd_mem_write32(swd_t *s, uint32_t addr, uint32_t v) { RETRY_OP(swd_mem_write32_once(s, addr, v)); }
int swd_mem_write16(swd_t *s, uint32_t addr, uint16_t v) { RETRY_OP(swd_mem_write16_once(s, addr, v)); }
int swd_mem_write8(swd_t *s, uint32_t addr, uint8_t v) { RETRY_OP(swd_mem_write8_once(s, addr, v)); }
static int read_block_retry(swd_t *s, uint32_t addr, uint32_t *buf, size_t words)
{
    RETRY_OP(swd_mem_read_block_once(s, addr, buf, words));
}

// Read the block twice (in chunks) and accept a chunk only when both reads agree, so a
// parity-valid but wrong sample on a noisy wire can't slip through.
#define CHECK_CHUNK 64
int swd_mem_read_block(swd_t *s, uint32_t addr, uint32_t *buf, size_t words)
{
    uint32_t again[CHECK_CHUNK];
    for (size_t done = 0; done < words;) {
        size_t n = words - done < CHECK_CHUNK ? words - done : CHECK_CHUNK;
        int ok = 0;
        for (int tries = 0; tries < 6 && !ok; tries++) {
            int rc = read_block_retry(s, addr + done * 4, buf + done, n);
            if (rc)
                return rc;
            if ((rc = read_block_retry(s, addr + done * 4, again, n)))
                return rc;
            ok = memcmp(buf + done, again, n * 4) == 0;
            if (!ok)
                s->glitches++;
        }
        if (!ok)
            return SWD_ERR_PARITY; // never got two identical reads
        done += n;
    }
    return SWD_OK;
}
