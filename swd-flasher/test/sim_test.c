// Host-side test: runs the real swd.c + gd32.c against a bit-level simulation of a
// GD32E230 SW-DP / MEM-AP / FMC. Build + run with `make -C test`.
//
// The simulator is written from the ARM ADIv5 spec and the GD32 FMC description, so it
// catches framing / parity / sequencing / logic bugs in the driver. It cannot catch
// misunderstandings of real silicon behaviour; that still needs a real board.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gd32.h"
#include "../src/mailbox.h"
#include "../src/swd.h"

#define FLASH_SIZE 65536
#define FLASH_BASE 0x08000000u
#define OB_BASE    0x1FFFF800u

typedef struct {
    // wire level
    int clk, host_out, host_drives, out_bit;
    uint32_t last16;
    int ones, in_reset, selected;
    // transaction state machine
    enum { P_IDLE, P_REQ, P_ACK, P_RD_DATA, P_WR_DATA } phase;
    int n, req_bits, ack, apndp, rnw, addr;
    uint32_t data, rdata;
    int e; // edges since request end
    // DAP
    uint32_t ctrl, select, csw, tar, rdbuff, sticky;
    // core / system
    uint32_t dhcsr_en, dhcsr_halt, demcr;
    uint32_t cpuid;
    int resets;
    // SRAM (0x20000000, 8 KB) for the mailbox test
    uint8_t ram[8192];
    // flash controller
    uint8_t flash[FLASH_SIZE];
    uint16_t ob[8];
    int protected_now, lk, obwen, busy_polls;
    uint32_t fmc_ctl, fmc_stat;
    // fault injection
    int wait_every, xfer_count, flip_read_parity_at, read_count, flip_all_reads;
    int present;
    int drop_program_at, program_count; // silently lose the Nth flash halfword write
    // statistics
    int errs;
} sim_t;

static int parity32(uint32_t v) { return __builtin_parity(v); }

static void sim_system_reset(sim_t *s)
{
    s->resets++;
    s->dhcsr_halt = (s->demcr & 1) ? 1 : 0;
    s->protected_now = (s->ob[0] & 0xFF) != 0xA5;
    s->lk = 1;
    s->obwen = 0;
    s->fmc_ctl = 0;
    s->busy_polls = 0;
    s->fmc_stat = 0;
}

static void flash_mass_erase(sim_t *s) { memset(s->flash, 0xFF, sizeof s->flash); }

static int mem_access(sim_t *s, int write, uint32_t addr, int size_bytes, uint32_t *v)
{
    // returns 0 ok, 1 fault (sets sticky err by caller)
    // for sub-word accesses, *v is in byte lanes as the MEM-AP presents them
    uint32_t a = addr & ~3u;

    if (a >= 0x20000000u && a < 0x20002000u) {
        uint8_t *p = &s->ram[a - 0x20000000u];
        if (write) memcpy(p, v, 4); else memcpy(v, p, 4);
        return 0;
    }
    if (a == 0xE000ED00) { if (!write) *v = s->cpuid; return 0; }
    if (a == 0xE000EDF0) {
        if (write) {
            if ((*v >> 16) == 0xA05F) {
                s->dhcsr_en = *v & 1;
                s->dhcsr_halt = (*v >> 1) & 1;
                if (!s->dhcsr_en) s->dhcsr_halt = 0;
            }
        } else {
            *v = (s->dhcsr_en) | (s->dhcsr_halt << 1) | ((s->dhcsr_en && s->dhcsr_halt) ? (1u << 17) : 0);
        }
        return 0;
    }
    if (a == 0xE000EDFC) { if (write) s->demcr = *v; else *v = s->demcr; return 0; }
    if (a == 0xE000ED0C) {
        if (write && *v == 0x05FA0004) sim_system_reset(s);
        else if (!write) *v = 0;
        return 0;
    }

    // ---- FMC
    if (a >= 0x40022000 && a < 0x40022100) {
        uint32_t off = a - 0x40022000;
        if (!write) {
            switch (off) {
            case 0x0C:
                *v = s->fmc_stat | (s->busy_polls > 0 ? 1 : 0);
                if (s->busy_polls > 0) s->busy_polls--;
                return 0;
            case 0x10: *v = s->fmc_ctl | (s->lk ? 0x80 : 0) | (s->obwen ? 0x200 : 0); return 0;
            case 0x1C: *v = s->protected_now ? 0x2 : 0; return 0;
            default: *v = 0; return 0;
            }
        }
        static int key_state_fmc, key_state_ob;
        switch (off) {
        case 0x04:
            if (key_state_fmc == 0 && *v == 0x45670123) key_state_fmc = 1;
            else if (key_state_fmc == 1 && *v == 0xCDEF89AB) { s->lk = 0; key_state_fmc = 0; }
            else key_state_fmc = 0;
            return 0;
        case 0x08:
            if (key_state_ob == 0 && *v == 0x45670123) key_state_ob = 1;
            else if (key_state_ob == 1 && *v == 0xCDEF89AB) { if (!s->lk) s->obwen = 1; key_state_ob = 0; }
            else key_state_ob = 0;
            return 0;
        case 0x0C: s->fmc_stat &= ~(*v & 0x34); return 0; // write 1 to clear PGERR/WPERR/ENDF
        case 0x10: {
            if (s->lk) { if (*v & 0x80) s->lk = 1; return 0; }
            uint32_t prev = s->fmc_ctl;
            s->fmc_ctl = *v & ~0x80u;
            if (*v & 0x80) { s->lk = 1; s->obwen = 0; }
            if ((*v & 0x40) && !(prev & 0x40)) { // START
                if (*v & 4) { // MER
                    if (s->protected_now) s->fmc_stat |= (1u << 4); // WPERR
                    else { flash_mass_erase(s); s->busy_polls = 3; s->fmc_stat |= 0x20; }
                } else if ((*v & 0x20) && s->obwen) { // OBER
                    if (s->protected_now) flash_mass_erase(s);
                    for (int i = 0; i < 8; i++) s->ob[i] = 0xFFFF;
                    s->busy_polls = 3;
                    s->fmc_stat |= 0x20;
                }
                s->fmc_ctl &= ~0x40u;
            }
            return 0;
        }
        default: return 0;
        }
    }
    // ---- flash array / option bytes
    if (a >= FLASH_BASE && a < FLASH_BASE + FLASH_SIZE) {
        uint32_t o = a - FLASH_BASE;
        if (!write) {
            uint32_t w = 0;
            if (!s->protected_now) memcpy(&w, &s->flash[o], 4);
            *v = w;
            return 0;
        }
        // GD32E23x: program 32-bit aligned words only (a halfword write -> PGAERR), PG set, not busy, target erased
        if (!(s->fmc_ctl & 1)) { s->fmc_stat |= 4; return 0; }
        if (size_bytes != 4 || (addr & 3)) { s->fmc_stat |= 8; return 0; } // PGAERR
        if (s->busy_polls > 0) { s->errs++; s->fmc_stat |= 4; return 0; } // driver forgot to poll BUSY
        uint32_t cur;
        memcpy(&cur, &s->flash[o], 4);
        if (cur != 0xFFFFFFFFu) { s->fmc_stat |= 4; return 0; }
        if (++s->program_count == s->drop_program_at) return 0; // write lost on the wire
        memcpy(&s->flash[o], v, 4);
        s->busy_polls = 2;
        s->fmc_stat |= 0x20;
        return 0;
    }
    if (a >= OB_BASE && a < OB_BASE + 16) {
        if (!write) { *v = s->ob[(addr - OB_BASE) / 2]; return 0; }
        if (!(s->fmc_ctl & 0x10) || !s->obwen) { s->fmc_stat |= 4; return 0; }
        if (size_bytes != 4 || (addr & 3)) { s->fmc_stat |= 8; return 0; } // E23x: word writes only
        if (s->busy_polls > 0) { s->errs++; s->fmc_stat |= 4; return 0; }
        uint32_t idx = (addr - OB_BASE) / 2;
        if (s->ob[idx] != 0xFFFF || s->ob[idx + 1] != 0xFFFF) { s->fmc_stat |= 4; return 0; }
        s->ob[idx] = (uint16_t)*v;
        s->ob[idx + 1] = (uint16_t)(*v >> 16);
        s->busy_polls = 2;
        return 0;
    }
    return 1;
}

static void ap_access(sim_t *s, int write, uint32_t reg, uint32_t *v)
{
    if (reg == 0x0) { if (write) s->csw = *v; else *v = s->csw; return; }
    if (reg == 0x4) { if (write) s->tar = *v; else *v = s->tar; return; }
    if (reg == 0xC) {
        int size = 1 << (s->csw & 7);
        if (write) {
            uint32_t val = *v;
            if (mem_access(s, 1, s->tar, size, &val)) s->sticky |= 0x20;
        } else {
            uint32_t out = s->rdbuff, val = 0;
            if (mem_access(s, 0, s->tar, size, &val)) { s->sticky |= 0x20; val = 0; }
            s->rdbuff = val >> ((s->tar & 3) * 8);
            if (size == 2) s->rdbuff &= 0xFFFF;
            *v = out;
        }
        if (s->csw & 0x30) s->tar += size;
        return;
    }
    *v = 0;
}

// returns ack
static int dap_transfer(sim_t *s, int apndp, int rnw, int addr, uint32_t *v)
{
    s->xfer_count++;
    if (s->wait_every && (s->xfer_count % s->wait_every) == 0) return 2; // WAIT
    int powered = (s->ctrl & 0x50000000) == 0x50000000;
    if (apndp) {
        if (!powered) return 4;
        ap_access(s, !rnw, addr, v);
        return 1;
    }
    if (rnw) {
        switch (addr) {
        case 0x0: *v = 0x0BE12477; break;
        case 0x4: *v = (s->ctrl & 0x50000000) | ((s->ctrl & 0x50000000) << 1) | s->sticky; break;
        case 0x8: *v = s->select; break;
        case 0xC: *v = s->rdbuff; s->rdbuff = 0; break; // like the real GD32E230: a repeat read gives 0
        }
    } else {
        switch (addr) {
        case 0x0: s->sticky &= ~(*v & 0x1E); break;
        case 0x4: s->ctrl = *v & 0x50000000; break;
        case 0x8: s->select = *v; break;
        }
    }
    return 1;
}

static void req_complete(sim_t *s)
{
    int r = s->req_bits;
    int start = r & 1, apndp = (r >> 1) & 1, rnw = (r >> 2) & 1;
    int a2 = (r >> 3) & 1, a3 = (r >> 4) & 1, par = (r >> 5) & 1, stop = (r >> 6) & 1, park = (r >> 7) & 1;
    if (!start || stop || !park || par != ((apndp + rnw + a2 + a3) & 1)) {
        // Real DPs silently ignore invalid frames (e.g. the ones of a line reset).
        s->phase = P_IDLE;
        return;
    }
    s->apndp = apndp;
    s->rnw = rnw;
    s->addr = (a3 << 3) | (a2 << 2);
    s->e = 0;
    s->data = 0;
    s->rdata = 0;
    s->ack = 0; // decided at the first ACK edge
    s->phase = P_ACK;
}

static void sim_edge(sim_t *s) // rising edge of SWCLK
{
    int hv = s->host_drives ? s->host_out : 1;

    s->last16 = ((s->last16 >> 1) | ((uint32_t)hv << 15)) & 0xFFFF;
    if ((s->phase == P_IDLE || s->phase == P_REQ) && s->host_drives) {
        // a released (floating) line is not host data: ignore it
        if (hv) s->ones++;
        else { if (s->ones >= 50) { s->in_reset = 1; } s->ones = 0; }
        if (s->ones >= 50) s->in_reset = 1;
    }
    if (s->last16 == 0xE79E && s->phase == P_IDLE) s->selected = 1;
    s->out_bit = 1;

    switch (s->phase) {
    case P_IDLE:
        if (s->in_reset) { if (!hv) s->in_reset = 0; break; }
        if (s->selected && s->present && s->host_drives && hv && s->ones == 1) { s->phase = P_REQ; s->n = 1; s->req_bits = 1; }
        break;
    case P_REQ:
        s->req_bits |= hv << s->n;
        if (++s->n == 8) req_complete(s);
        break;
    case P_ACK: {
        s->e++;
        if (s->e == 1) {
            uint32_t v = 0;
            if (s->rnw) {
                s->ack = dap_transfer(s, s->apndp, 1, s->addr, &v);
                s->data = v;
                if (s->ack == 1) s->read_count++;
            } else {
                s->ack = -1; // write: executed after data arrives; WAIT/FAULT decided now
                uint32_t dummy = 0;
                // decide ack without side effects: re-use dap_transfer for AP/DP checks on completion
                s->ack = 1;
                int powered = (s->ctrl & 0x50000000) == 0x50000000;
                s->xfer_count++;
                if (s->wait_every && (s->xfer_count % s->wait_every) == 0) s->ack = 2;
                else if (s->apndp && !powered) s->ack = 4;
                (void)dummy;
            }
            s->out_bit = s->ack & 1;
        } else if (s->e == 2) s->out_bit = (s->ack >> 1) & 1;
        else if (s->e == 3) s->out_bit = (s->ack >> 2) & 1;
        else { // e == 4
            if (s->ack != 1) { s->phase = P_IDLE; s->ones = 0; break; }
            if (s->rnw) { s->phase = P_RD_DATA; s->e = 0; s->out_bit = s->data & 1; s->n = 1; }
            else { s->phase = P_WR_DATA; s->e = 0; s->n = 0; }
        }
        break;
    }
    case P_RD_DATA:
        if (s->n < 32) s->out_bit = (s->data >> s->n) & 1;
        else if (s->n == 32) {
            int p = parity32(s->data);
            if ((s->flip_read_parity_at && s->read_count == s->flip_read_parity_at) || s->flip_all_reads) p ^= 1;
            s->out_bit = p;
        } else { s->phase = P_IDLE; s->ones = 0; s->out_bit = 1; break; }
        s->n++;
        break;
    case P_WR_DATA:
        // edge 0 is the turnaround cycle (host not yet driving data)
        s->e++;
        if (s->e == 1) break;
        {
            int idx = s->e - 2; // 0..31 data, 32 parity
            if (idx < 32) s->data |= (uint32_t)hv << idx;
            else {
                if (hv != parity32(s->data)) { printf("SIM: write parity error\n"); s->errs++; }
                else {
                    uint32_t v = s->data;
                    // execute: undo the xfer_count double count by temporarily adjusting
                    int save = s->wait_every;
                    s->wait_every = 0;
                    dap_transfer(s, s->apndp, 0, s->addr, &v);
                    s->wait_every = save;
                }
                s->phase = P_IDLE;
                s->ones = 0;
            }
        }
        break;
    }
}

// ------------------------------------------------------------------ wire vtable
static void io_clk(void *c, int level)
{
    sim_t *s = c;
    int rising = !s->clk && level;
    s->clk = level;
    if (rising) sim_edge(s);
}
static void io_dio_write(void *c, int level) { ((sim_t *)c)->host_out = level; }
static void io_dio_mode(void *c, int out) { ((sim_t *)c)->host_drives = out; }
static int io_dio_read(void *c)
{
    sim_t *s = c;
    return s->host_drives ? s->host_out : s->out_bit;
}
static void io_delay(void *c) { (void)c; }
static void io_delay_ms(void *c, uint32_t ms) { (void)c; (void)ms; }

static const swd_io_t IO = {io_clk, io_dio_write, io_dio_mode, io_dio_read, NULL, io_delay, io_delay_ms};

// ------------------------------------------------------------------ tests
static void sim_reset_chip(sim_t *s, int protected_chip, int present)
{
    memset(s, 0, sizeof *s);
    s->out_bit = 1;
    s->cpuid = 0x410DD200;
    s->present = present;
    for (size_t i = 0; i < FLASH_SIZE; i++) s->flash[i] = (uint8_t)(i * 7 + 3); // "old firmware"
    for (int i = 0; i < 8; i++) s->ob[i] = 0x00FF;
    s->ob[0] = protected_chip ? 0x33CC : 0x5AA5;
    s->protected_now = protected_chip;
    s->lk = 1;
    s->fmc_ctl = 0;
}

static uint8_t *make_image(size_t len)
{
    uint8_t *img = malloc(len);
    srand(42);
    for (size_t i = 0; i < len; i++) img[i] = (rand() % 5 == 0) ? 0xFF : (uint8_t)rand();
    img[0] = 0x00; img[1] = 0x20; img[2] = 0x00; img[3] = 0x20; // SP 0x20000000
    return img;
}

static int g_fail;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); g_fail++; } else printf("ok:   %s\n", msg); } while (0)

static int installing_calls;
static void on_installing(void *u) { (void)u; installing_calls++; }

int main(void)
{
    static sim_t sim;
    swd_t swd;
    setvbuf(stdout, NULL, _IONBF, 0);
    gd32_cb_t cb = {on_installing, NULL, "sim"};
    size_t len = 20001;
    uint8_t *img = make_image(len);

    // 1. protected chip: first call unlocks, second programs
    printf("--- protected chip\n");
    sim_reset_chip(&sim, 1, 1);
    swd_init(&swd, &IO, &sim);
    gd32_result_t r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_UNLOCKED_RETRY, "protected chip -> UNLOCKED_RETRY");
    CHECK(sim.ob[0] == 0x5AA5, "option bytes now unprotected");
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_OK, "second pass programs OK");
    CHECK(memcmp(sim.flash, img, len) == 0, "flash content equals image");
    CHECK(sim.flash[len] == 0xFF, "flash beyond image erased");
    CHECK(sim.errs == 0, "no protocol errors / BUSY violations");
    CHECK(!sim.dhcsr_en && !sim.dhcsr_halt, "target left running");

    // 1b. a flash write silently lost on the wire (seen on real hardware) is re-programmed
    printf("--- lost flash write is repaired\n");
    sim_reset_chip(&sim, 0, 1);
    sim.drop_program_at = 2900;
    swd_init(&swd, &IO, &sim);
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_OK, "lost write -> repaired -> OK");
    CHECK(memcmp(sim.flash, img, len) == 0, "flash content equals image after repair");
    CHECK(sim.errs == 0, "no BUSY violations while repairing");

    // 2. unlocked chip, with WAIT responses injected
    printf("--- unprotected chip + WAIT injection\n");
    sim_reset_chip(&sim, 0, 1);
    sim.wait_every = 7;
    swd_init(&swd, &IO, &sim);
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_OK, "programs OK despite WAITs");
    CHECK(memcmp(sim.flash, img, len) == 0, "flash content equals image");
    CHECK(sim.errs == 0, "no protocol errors");

    // 3. nothing connected
    printf("--- no target\n");
    sim_reset_chip(&sim, 0, 0);
    swd_init(&swd, &IO, &sim);
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_NO_TARGET, "no target -> NO_TARGET");

    // 4. wrong core
    printf("--- wrong core\n");
    sim_reset_chip(&sim, 0, 1);
    sim.cpuid = 0x410FC231; // Cortex-M3
    swd_init(&swd, &IO, &sim);
    int before = installing_calls;
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r == GD32_WRONG_CORE, "Cortex-M3 refused");
    CHECK(installing_calls == before, "no install attempted");
    CHECK(sim.flash[1] == (uint8_t)(1 * 7 + 3), "flash untouched");

    // 5. corrupted reads: single glitches are recovered, persistent corruption must fail
    printf("--- parity corruption\n");
    int recovered = 0;
    int positions[] = {3, 20, 60, 300, 1500, 4000};
    for (unsigned k = 0; k < sizeof positions / sizeof positions[0]; k++) {
        sim_reset_chip(&sim, 0, 1);
        swd_init(&swd, &IO, &sim);
        sim.flip_read_parity_at = positions[k];
        r = gd32_flash(&swd, img, len, 1, &cb);
        int flash_ok = memcmp(sim.flash, img, len) == 0;
        printf("      flip read #%d -> result %d, flash %s\n", positions[k], (int)r, flash_ok ? "correct" : "NOT correct");
        CHECK(r != GD32_OK || flash_ok, "never reports OK with wrong flash");
        if (r == GD32_OK && flash_ok) recovered++;
    }
    CHECK(recovered > 0, "single parity glitches are recovered by resync+retry");
    sim_reset_chip(&sim, 0, 1);
    swd_init(&swd, &IO, &sim);
    sim.flip_all_reads = 1;
    r = gd32_flash(&swd, img, len, 1, &cb);
    CHECK(r != GD32_OK, "persistent corruption is reported, not accepted");

    // 6. unlock disabled
    printf("--- unlock disabled\n");
    sim_reset_chip(&sim, 1, 1);
    swd_init(&swd, &IO, &sim);
    r = gd32_flash(&swd, img, len, 0, &cb);
    CHECK(r == GD32_ERROR, "protected + unlock disabled -> ERROR");
    CHECK(sim.flash[10] == (uint8_t)(10 * 7 + 3), "flash untouched");

    // 7. master mailbox over SWD: the real mailbox.c writes into simulated SRAM
    printf("--- master mailbox\n");
    sim_reset_chip(&sim, 0, 1);
    swd_init(&swd, &IO, &sim);
    CHECK(swd_connect(&swd) == SWD_OK, "connect to simulated controller");
    mbx_t mb;
    uint32_t magic_ram = 0x3258424Du;   // 'MBX2', written by the firmware at boot
    memcpy(&sim.ram[MBX_ADDR - 0x20000000u], &magic_ram, 4);
    uint32_t tele[MBX_WORDS] = {0};
    tele[0] = magic_ram;
    float vbus = 24.1f;
    memcpy(&tele[0x28 / 4], &vbus, 4);
    tele[0x48 / 4] = 5;                 // hallSlave
    tele[0x50 / 4] = 12345;             // uptimeMs
    memcpy(&sim.ram[MBX_ADDR - 0x20000000u], tele, sizeof tele);
    CHECK(mbx_read(&swd, &mb) == SWD_OK && mb.magic == MBX_MAGIC, "mailbox magic readable");
    CHECK(mb.vBus == 24.1f && mb.hallSlave == 5 && mb.uptimeMs == 12345, "telemetry read at the firmware offsets");
    CHECK(mbx_write_command(&swd, 7, -150, 80, 1) == SWD_OK, "command frame written");
    uint32_t w[MBX_WORDS];
    memcpy(w, &sim.ram[MBX_ADDR - 0x20000000u], sizeof w);
    CHECK((int32_t)w[2] == -150 && (int32_t)w[3] == 80 && w[4] == 1 && w[1] == 7,
          "cmdMaster, cmdSlave, enable and heartbeat land at the firmware offsets");
    CHECK(mbx_read(&swd, &mb) == SWD_OK && mb.cmdMaster == -150 && mb.cmdSlave == 80 && mb.enable == 1 &&
              mb.heartbeat == 7, "mbx_read sees what mbx_write_command wrote");
    CHECK(mbx_write_led(&swd, MBX_LED_RED | MBX_LED_LOWER, 0xFF) == SWD_OK, "LED frame written");
    CHECK(mbx_read(&swd, &mb) == SWD_OK && mb.ledMaster == (MBX_LED_RED | MBX_LED_LOWER) && mb.ledSlave == MBX_LED_ALL,
          "LED masks read back (slave mask clipped to the 5 LED bits)");
    CHECK(mbx_power_off(&swd) == SWD_OK && mbx_read(&swd, &mb) == SWD_OK && mb.power == MBX_POWER_OFF_KEY,
          "power-off key written at 0x1C");
    CHECK(MBX_BYTES == 0x54, "mailbox size matches the firmware (0x54)");

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    free(img);
    return g_fail != 0;
}
