#include "gd32.h"

#include <stdio.h>
#include <string.h>

#ifndef GD32_LOG
#define GD32_LOG(...) printf(__VA_ARGS__)
#endif

// ---- Cortex-M system control / debug registers
#define CPUID  0xE000ED00u
#define AIRCR  0xE000ED0Cu
#define DHCSR  0xE000EDF0u
#define DEMCR  0xE000EDFCu

#define DHCSR_KEY      0xA05F0000u
#define DHCSR_DEBUGEN  (1u << 0)
#define DHCSR_HALT     (1u << 1)
#define DHCSR_S_HALT   (1u << 17)
#define DEMCR_VC_CORERESET (1u << 0)
#define AIRCR_KEY_RESET 0x05FA0004u // VECTKEY | SYSRESETREQ

#define CPUID_PARTNO(x) (((x) >> 4) & 0xFFF)
#define PARTNO_CM23 0xD20

// ---- GD32E230 flash controller (FMC)
#define FMC_BASE   0x40022000u
#define FMC_KEY    (FMC_BASE + 0x04)
#define FMC_OBKEY  (FMC_BASE + 0x08)
#define FMC_STAT   (FMC_BASE + 0x0C)
#define FMC_CTL    (FMC_BASE + 0x10)
#define FMC_OBSTAT (FMC_BASE + 0x1C)

#define FMC_KEY1 0x45670123u
#define FMC_KEY2 0xCDEF89ABu

#define STAT_BUSY  (1u << 0)
#define STAT_PGERR (1u << 2)
#define STAT_WPERR (1u << 4)
#define STAT_ENDF  (1u << 5)
#define STAT_PGAERR (1u << 3) // GD32E23x: program alignment error (e.g. a halfword write)
#define STAT_ERRS  (STAT_PGERR | STAT_PGAERR | STAT_WPERR)

#define CTL_PG    (1u << 0)
#define CTL_MER   (1u << 2)
#define CTL_OBPG  (1u << 4)
#define CTL_OBER  (1u << 5)
#define CTL_START (1u << 6)
#define CTL_LK    (1u << 7)
#define CTL_OBWEN (1u << 9)

#define OBSTAT_PROT_MASK 0x6u // SPC / PLEVEL field: non-zero = read protected

#define FLASH_BASE  0x08000000u
#define FLASH_MAX   (64u * 1024u) // GD32E230x8
#define OB_BASE     0x1FFFF800u

#define TRY(x)                      \
    do {                            \
        int _rc = (x);              \
        if (_rc) {                  \
            GD32_LOG("swd: %s failed: %s (line %d)\n", #x, swd_strerror(_rc), __LINE__); \
            return GD32_ERROR;      \
        }                           \
    } while (0)

static int wait_not_busy(swd_t *s, uint32_t *stat_out, uint32_t timeout_polls)
{
    uint32_t st = 0;
    for (uint32_t i = 0; i < timeout_polls; i++) {
        int rc = swd_mem_read32(s, FMC_STAT, &st);
        if (rc)
            return rc;
        if (!(st & STAT_BUSY)) {
            if (stat_out)
                *stat_out = st;
            return SWD_OK;
        }
        s->io->delay_ms(s->ctx, 1);
    }
    return SWD_ERR_TIMEOUT;
}

static int fmc_unlock(swd_t *s)
{
    uint32_t ctl;
    int rc = swd_mem_read32(s, FMC_CTL, &ctl);
    if (rc)
        return rc;
    if (!(ctl & CTL_LK))
        return SWD_OK;
    if ((rc = swd_mem_write32(s, FMC_KEY, FMC_KEY1)))
        return rc;
    if ((rc = swd_mem_write32(s, FMC_KEY, FMC_KEY2)))
        return rc;
    if ((rc = swd_mem_read32(s, FMC_CTL, &ctl)))
        return rc;
    return (ctl & CTL_LK) ? SWD_ERR_PROTO : SWD_OK;
}

static int fmc_clear_flags(swd_t *s)
{
    return swd_mem_write32(s, FMC_STAT, STAT_ERRS | STAT_ENDF);
}

static int halt_core(swd_t *s)
{
    int rc = swd_mem_write32(s, DHCSR, DHCSR_KEY | DHCSR_DEBUGEN | DHCSR_HALT);
    if (rc)
        return rc;
    for (int i = 0; i < 50; i++) {
        uint32_t d;
        if ((rc = swd_mem_read32(s, DHCSR, &d)))
            return rc;
        if (d & DHCSR_S_HALT)
            return SWD_OK;
        s->io->delay_ms(s->ctx, 2);
    }
    return SWD_ERR_TIMEOUT;
}

// Reset the chip and catch the core at the reset vector, before the application
// (and its watchdog) can run. Reconnects afterwards.
static gd32_result_t reset_halt(swd_t *s)
{
    TRY(swd_mem_write32(s, DEMCR, DEMCR_VC_CORERESET));
    TRY(swd_mem_write32(s, AIRCR, AIRCR_KEY_RESET));
    s->io->delay_ms(s->ctx, 20);
    TRY(swd_connect(s));
    TRY(swd_mem_write32(s, DEMCR, DEMCR_VC_CORERESET));
    TRY(halt_core(s));
    return GD32_OK;
}

static void run_target(swd_t *s)
{
    // Resume and restart so the new firmware boots from reset.
    swd_mem_write32(s, DEMCR, 0);
    swd_mem_write32(s, DHCSR, DHCSR_KEY); // C_DEBUGEN = 0, C_HALT = 0
    swd_mem_write32(s, AIRCR, AIRCR_KEY_RESET);
}

// Remove read protection: erase option bytes (which mass-erases a protected chip),
// then program the "no protection" option byte set, then reset.
static gd32_result_t unlock_chip(swd_t *s)
{
    uint32_t st, ctl;
    GD32_LOG("target is read-protected: erasing option bytes (this wipes the old firmware)\n");
    TRY(fmc_unlock(s));
    TRY(swd_mem_write32(s, FMC_OBKEY, FMC_KEY1));
    TRY(swd_mem_write32(s, FMC_OBKEY, FMC_KEY2));
    TRY(swd_mem_read32(s, FMC_CTL, &ctl));
    if (!(ctl & CTL_OBWEN)) {
        GD32_LOG("option byte write enable did not set (CTL=0x%08x)\n", (unsigned)ctl);
        return GD32_ERROR;
    }
    TRY(fmc_clear_flags(s));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_OBER | CTL_OBWEN));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_OBER | CTL_OBWEN | CTL_START));
    TRY(wait_not_busy(s, &st, 3000));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_OBWEN));

    // GD32E23x option bytes are programmed as 32-bit words. Like the vendor's ob_erase(): write
    // SPC = 0xA5 (no protection) with its complement, USER left at 0xFF: 0xFFFF5AA5 at 0x1FFFF800.
    TRY(fmc_clear_flags(s));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_OBPG | CTL_OBWEN));
    TRY(swd_mem_write32(s, OB_BASE, 0xFFFF5AA5u));
    TRY(wait_not_busy(s, &st, 200));
    if (st & STAT_ERRS) {
        GD32_LOG("option byte program error, STAT=0x%08x\n", (unsigned)st);
        return GD32_ERROR;
    }
    TRY(swd_mem_write32(s, FMC_CTL, CTL_OBWEN));
    // Option bytes are reloaded on system reset
    TRY(swd_mem_write32(s, AIRCR, AIRCR_KEY_RESET));
    s->io->delay_ms(s->ctx, 50);
    return GD32_UNLOCKED_RETRY;
}

static gd32_result_t mass_erase(swd_t *s)
{
    uint32_t st;
    TRY(fmc_unlock(s));
    TRY(fmc_clear_flags(s));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_MER));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_MER | CTL_START));
    TRY(wait_not_busy(s, &st, 5000));
    TRY(swd_mem_write32(s, FMC_CTL, 0));
    if (st & STAT_ERRS) {
        GD32_LOG("mass erase error, STAT=0x%08x\n", (unsigned)st);
        return GD32_ERROR;
    }
    return GD32_OK;
}

// Image word at byte offset `off` (4-aligned), padded with erased bytes past the end.
static uint32_t image_word(const uint8_t *image, size_t len, size_t off)
{
    uint32_t w = 0;
    for (int b = 0; b < 4; b++)
        w |= (uint32_t)(off + b < len ? image[off + b] : 0xFF) << (8 * b);
    return w;
}

// GD32E23x main flash is programmed in 32-bit words (the vendor library has only word and
// doubleword programming; a halfword write may be rejected with PGAERR).
static gd32_result_t program_word(swd_t *s, size_t off, uint32_t w)
{
    uint32_t st;
    TRY(swd_mem_write32(s, FLASH_BASE + off, w));
    TRY(wait_not_busy(s, &st, 200));
    if (st & STAT_ERRS) {
        GD32_LOG("program error at 0x%08x, STAT=0x%08x\n", (unsigned)(FLASH_BASE + off), (unsigned)st);
        swd_mem_write32(s, FMC_CTL, CTL_LK);
        return GD32_ERROR;
    }
    return GD32_OK;
}

static gd32_result_t program(swd_t *s, const uint8_t *image, size_t len)
{
    gd32_result_t r;
    TRY(fmc_unlock(s));
    TRY(fmc_clear_flags(s));
    TRY(swd_mem_write32(s, FMC_CTL, CTL_PG));
    for (size_t off = 0; off < len; off += 4) {
        uint32_t w = image_word(image, len, off);
        if (w == 0xFFFFFFFFu)
            continue; // erased state already
        if ((r = program_word(s, off, w)) != GD32_OK)
            return r;
        if ((off & 0x7FF) == 0)
            s->io->delay_ms(s->ctx, 1); // let the host's other tasks run (ESP32 task watchdog)
    }
    TRY(swd_mem_write32(s, FMC_CTL, CTL_LK));
    return GD32_OK;
}

// Safety net for writes lost on a noisy SWD wire. Flash bits only go 1 -> 0, so a lost word
// reads back erased (0xFFFFFFFF) and can safely be programmed again. A word holding any other
// wrong value can't be repaired without an erase -> error. *repaired = words re-programmed.
static gd32_result_t repair(swd_t *s, const uint8_t *image, size_t len, int *repaired)
{
    uint32_t buf[64];
    gd32_result_t r;
    int pg_on = 0;
    *repaired = 0;
    size_t words = (len + 3) / 4;
    for (size_t w = 0; w < words; w += 64) {
        size_t n = words - w > 64 ? 64 : words - w;
        TRY(swd_mem_read_block(s, FLASH_BASE + w * 4, buf, n));
        for (size_t i = 0; i < n; i++) {
            size_t off = (w + i) * 4;
            uint32_t want = image_word(image, len, off);
            if (buf[i] == want)
                continue;
            if (buf[i] != 0xFFFFFFFFu) {
                GD32_LOG("flash at 0x%08x is 0x%08x, expected 0x%08x: needs an erase\n", (unsigned)(FLASH_BASE + off),
                         (unsigned)buf[i], (unsigned)want);
                if (pg_on)
                    swd_mem_write32(s, FMC_CTL, CTL_LK);
                return GD32_ERROR;
            }
            if (!pg_on) {
                TRY(fmc_unlock(s));
                TRY(fmc_clear_flags(s));
                TRY(swd_mem_write32(s, FMC_CTL, CTL_PG));
                pg_on = 1;
            }
            GD32_LOG("re-programming lost word at 0x%08x\n", (unsigned)(FLASH_BASE + off));
            if ((r = program_word(s, off, want)) != GD32_OK)
                return r;
            (*repaired)++;
        }
    }
    if (pg_on)
        TRY(swd_mem_write32(s, FMC_CTL, CTL_LK));
    return GD32_OK;
}

static gd32_result_t verify(swd_t *s, const uint8_t *image, size_t len)
{
    uint32_t buf[64];
    size_t words = (len + 3) / 4;
    for (size_t w = 0; w < words;) {
        size_t n = words - w > 64 ? 64 : words - w;
        TRY(swd_mem_read_block(s, FLASH_BASE + w * 4, buf, n));
        for (size_t i = 0; i < n; i++) {
            uint32_t expect = 0xFFFFFFFFu;
            size_t o = (w + i) * 4;
            for (int b = 0; b < 4; b++)
                if (o + b < len)
                    expect = (expect & ~(0xFFu << (8 * b))) | ((uint32_t)image[o + b] << (8 * b));
            if (buf[i] != expect) {
                GD32_LOG("verify mismatch at 0x%08x: flash=0x%08x expected=0x%08x\n",
                         (unsigned)(FLASH_BASE + o), (unsigned)buf[i], (unsigned)expect);
                return GD32_ERROR;
            }
        }
        w += n;
    }
    return swd_check_sticky(s) ? GD32_ERROR : GD32_OK;
}

gd32_result_t gd32_flash(swd_t *s, const uint8_t *image, size_t len, int allow_unlock,
                         const gd32_cb_t *cb)
{
    uint32_t v;
    int rc = swd_connect(s);
    if (rc == SWD_ERR_NOTARGET)
        return GD32_NO_TARGET;
    if (rc) {
        GD32_LOG("%s: SWD connect failed: %s\n", cb->name, swd_strerror(rc));
        return GD32_NO_TARGET; // a flaky link is treated as "not there yet"
    }
    GD32_LOG("%s: SWD up, DPIDR=0x%08x\n", cb->name, (unsigned)s->dpidr);

    TRY(swd_mem_read32(s, CPUID, &v));
    GD32_LOG("%s: CPUID=0x%08x\n", cb->name, (unsigned)v);
    if (CPUID_PARTNO(v) != PARTNO_CM23) {
        GD32_LOG("%s: not a Cortex-M23 (GD32E230 expected), refusing to program\n", cb->name);
        return GD32_WRONG_CORE;
    }

    if (cb->installing)
        cb->installing(cb->user);

    TRY(halt_core(s));
    gd32_result_t r = reset_halt(s);
    if (r != GD32_OK)
        return r;

    TRY(swd_mem_read32(s, FMC_OBSTAT, &v));
    GD32_LOG("%s: FMC_OBSTAT=0x%08x\n", cb->name, (unsigned)v);
    if (v & OBSTAT_PROT_MASK) {
        if (!allow_unlock) {
            GD32_LOG("%s: read-protected and unlocking is disabled\n", cb->name);
            return GD32_ERROR;
        }
        return unlock_chip(s);
    }

    if (len == 0 || len > FLASH_MAX) {
        GD32_LOG("%s: bad image size %u\n", cb->name, (unsigned)len);
        return GD32_ERROR;
    }

    GD32_LOG("%s: erasing\n", cb->name);
    if ((r = mass_erase(s)) != GD32_OK)
        return r;
    GD32_LOG("%s: programming %u bytes\n", cb->name, (unsigned)len);
    if ((r = program(s, image, len)) != GD32_OK)
        return r;
    for (int pass = 0; pass < 3; pass++) { // read back; re-program halfwords lost on the wire
        int fixed = 0;
        if ((r = repair(s, image, len, &fixed)) != GD32_OK)
            return r;
        if (!fixed)
            break;
        GD32_LOG("%s: re-programmed %d lost word(s)\n", cb->name, fixed);
    }
    GD32_LOG("%s: verifying\n", cb->name);
    if ((r = verify(s, image, len)) != GD32_OK)
        return r;

    run_target(s);
    GD32_LOG("%s: done\n", cb->name);
    return GD32_OK;
}
