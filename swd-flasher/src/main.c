// Hoverboard motor-controller flasher for the ESP32-C6.
//
// On boot it repeatedly tries to program each connected GD32E230 over SWD until it works:
//   LED blue (blinking)  = looking for a controller
//   LED yellow           = erasing / programming / verifying
//   LED green            = all controllers programmed and verified
//   LED red              = error (shown briefly, then it retries)
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
#include "esp_swd_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gd32.h"
#include "led.h"
#include "swd.h"

static const char *TAG = "flasher";

// Images compiled in from firmware/target_a.bin and firmware/target_b.bin (src/fw_images.c)
extern const unsigned char fw_a[];
extern const unsigned int fw_a_len;
extern const unsigned char fw_b[];
extern const unsigned int fw_b_len;

typedef enum { TS_SEARCH, TS_INSTALL, TS_DONE, TS_ERROR } tstate_t;

typedef struct {
    const char *name;
    int swclk, swdio, nrst;
    const uint8_t *image;
    size_t len;
    esp_swd_pins_t pins;
    swd_t swd;
    tstate_t state;
    int unlock_tries;
    int image_ok;
    TickType_t last_log;
    int have_done;         // done_uid is valid
    uint32_t done_uid[3];  // unique ID of the board last finished on this connector
    int lost;              // consecutive failed presence checks while done
    TickType_t last_watch;
} target_t;

_Static_assert(NUM_TARGETS >= 1 && NUM_TARGETS <= 2, "NUM_TARGETS must be 1 or 2");

static target_t T[NUM_TARGETS];

static void refresh_led(void)
{
    int installing = 0, error = 0, done = 0;
    for (int i = 0; i < NUM_TARGETS; i++) {
        installing |= T[i].state == TS_INSTALL;
        error |= T[i].state == TS_ERROR;
        done += T[i].state == TS_DONE;
    }
    if (installing) led_set(LED_INSTALL_YELLOW);
    else if (error) led_set(LED_ERROR_RED);
    else if (done == NUM_TARGETS) led_set(LED_DONE_GREEN);
    else led_set(LED_SEARCH_BLUE);
}

static void set_state(target_t *t, tstate_t s)
{
    t->state = s;
    refresh_led();
}

static void on_installing(void *user) { set_state((target_t *)user, TS_INSTALL); }

// Cortex-M image sanity check: initial SP in SRAM, reset vector in flash with Thumb bit.
static int check_image(const char *name, const uint8_t *img, size_t len)
{
    if (len == 0) {
        ESP_LOGW(TAG, "%s: no firmware image embedded, backup mode (put firmware/target_a.bin / target_b.bin in place and rebuild)", name);
        return 0;
    }
    if (len < 8 || len > 64 * 1024) {
        ESP_LOGE(TAG, "%s: image size %u bytes is not plausible for a GD32E230 (8 B - 64 KB)", name, (unsigned)len);
        return 0;
    }
    uint32_t sp, rv;
    memcpy(&sp, img, 4);
    memcpy(&rv, img + 4, 4);
    if (sp < 0x20000000u || sp > 0x20002000u || !(rv & 1) || rv < 0x08000000u || rv > 0x0800FFFFu) {
        ESP_LOGE(TAG, "%s: vector table looks wrong (SP=0x%08x reset=0x%08x): is this a GD32E230 image?",
                 name, (unsigned)sp, (unsigned)rv);
        return 0;
    }
    return 1;
}

static void log_search(target_t *t)
{
    TickType_t now = xTaskGetTickCount();
    if (t->last_log && now - t->last_log < pdMS_TO_TICKS(3000))
        return;
    t->last_log = now;
    const char *why = t->swd.last_ack == 7 ? "nobody drives SWDIO (check power, SWCLK/SWDIO wiring and swap them)"
                      : t->swd.last_ack == 0 ? "SWDIO stuck low (short to GND?)"
                      : "unexpected reply";
    ESP_LOGW(TAG, "%s: no controller on SWD: ACK=%d%d%d id=0x%08x -> %s", t->name, t->swd.last_ack & 1,
             (t->swd.last_ack >> 1) & 1, (t->swd.last_ack >> 2) & 1, (unsigned)t->swd.last_id, why);
}

// Read-only look at a chip (nothing is halted or written) and, if enabled, dump its flash.
// Returns 0 on success, -1 if a read failed.
static int probe_and_dump(target_t *t)
{
        uint32_t cpuid = 0, obstat = 0;
        int r1 = swd_mem_read32(&t->swd, 0xE000ED00u, &cpuid);
        int r2 = swd_mem_read32(&t->swd, 0x4002201Cu, &obstat);
        ESP_LOGI(TAG, "%s: CPUID=0x%08x (%s) FMC_OBSTAT=0x%08x (%s) [rc %d/%d]", t->name, (unsigned)cpuid,
                 ((cpuid >> 4) & 0xFFF) == 0xD20 ? "Cortex-M23 as expected" : "NOT a Cortex-M23",
                 (unsigned)obstat, (obstat & 6) ? "read-protected" : "not protected", r1, r2);
        // GPIO state of the running stock firmware (read-only). Mode per pin: I=input O=output A=alt-func N=analog
        static const struct { const char *n; uint32_t base; } ports[] = {
            {"A", 0x48000000u}, {"B", 0x48000400u}, {"C", 0x48000800u}, {"F", 0x48001400u}};
        for (unsigned k = 0; k < 4; k++) {
            uint32_t ctl = 0, pud = 0, ist = 0, oct = 0;
            int rr = swd_mem_read32(&t->swd, ports[k].base + 0x00, &ctl);
            rr |= swd_mem_read32(&t->swd, ports[k].base + 0x0C, &pud);
            rr |= swd_mem_read32(&t->swd, ports[k].base + 0x10, &ist);
            rr |= swd_mem_read32(&t->swd, ports[k].base + 0x14, &oct);
            char modes[17];
            for (int pin = 0; pin < 16; pin++)
                modes[pin] = "IOAN"[(ctl >> (2 * pin)) & 3];
            modes[16] = 0;
            ESP_LOGI(TAG, "GPIO%s modes(pin0..15)=%s in=0x%04x out=0x%04x pud=0x%08x rc=%d", ports[k].n, modes,
                     (unsigned)(ist & 0xFFFF), (unsigned)(oct & 0xFFFF), (unsigned)pud, rr);
        }
        uint32_t t0ctl1 = 0, t0chctl2 = 0, t0cchp = 0, t0car = 0, t0psc = 0;
        swd_mem_read32(&t->swd, 0x40012C00u + 0x04, &t0ctl1);
        swd_mem_read32(&t->swd, 0x40012C00u + 0x20, &t0chctl2);
        swd_mem_read32(&t->swd, 0x40012C00u + 0x44, &t0cchp);
        swd_mem_read32(&t->swd, 0x40012C00u + 0x2C, &t0car);
        swd_mem_read32(&t->swd, 0x40012C00u + 0x28, &t0psc);
        ESP_LOGI(TAG, "TIMER0 CTL1=0x%08x CHCTL2=0x%08x CCHP=0x%08x CAR=%u PSC=%u", (unsigned)t0ctl1,
                 (unsigned)t0chctl2, (unsigned)t0cchp, (unsigned)t0car, (unsigned)t0psc);
#if DUMP_STOCK_FLASH
        {
            uint32_t crc = 0xFFFFFFFFu, uid[3] = {0}, fsz = 0;
            int rcu = swd_mem_read_block(&t->swd, 0x1FFFF7ACu, uid, 3);
            printf("DUMP uid read rc=%d (%s)\n", rcu, swd_strerror(rcu));
            swd_mem_read32(&t->swd, 0x1FFFF7E0u, &fsz);
            printf("DUMP BEGIN uid=%08x%08x%08x flashsize_kb=%u\n", (unsigned)uid[2], (unsigned)uid[1],
                   (unsigned)uid[0], (unsigned)(fsz & 0xFFFF));
            uint32_t buf[32];
            int bad = 0;
            for (uint32_t off = 0; off < 0x10000 && !bad; off += sizeof buf) {
                { int rcb = swd_mem_read_block(&t->swd, 0x08000000u + off, buf, 32);
                  if (rcb) { printf("DUMP block read at 0x%05x rc=%d (%s) last ACK bits=%d%d%d\n", (unsigned)off, rcb, swd_strerror(rcb), t->swd.last_ack & 1, (t->swd.last_ack >> 1) & 1, (t->swd.last_ack >> 2) & 1); bad = 1; break; } }
                printf("DUMP %05x", (unsigned)off);
                for (int w = 0; w < 32; w++) {
                    printf(" %08x", (unsigned)buf[w]);
                    const uint8_t *b = (const uint8_t *)&buf[w];
                    for (int k = 0; k < 4; k++) { // CRC-32 (IEEE), bitwise
                        crc ^= b[k];
                        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
                    }
                }
                printf("\n");
                vTaskDelay(1); // let the USB console drain
            }
            printf("DUMP END %s crc32=%08x glitches=%u\n", bad ? "ERROR" : "OK", (unsigned)~crc, (unsigned)t->swd.glitches);
            if (bad) return -1;
        }
#endif
    return (r1 || r2) ? -1 : 0;
}

static void show_error(target_t *t)
{
    set_state(t, TS_ERROR);
    vTaskDelay(pdMS_TO_TICKS(ERROR_HOLD_MS));
    set_state(t, TS_SEARCH);
}

static void uid_str(const uint32_t u[3], char out[25])
{
    snprintf(out, 25, "%08x%08x%08x", (unsigned)u[2], (unsigned)u[1], (unsigned)u[0]);
}

static void finish(target_t *t, const uint32_t uid[3], int uid_ok)
{
    t->have_done = uid_ok;
    if (uid_ok)
        memcpy(t->done_uid, uid, sizeof t->done_uid);
    t->lost = 0;
    t->last_watch = xTaskGetTickCount();
    set_state(t, TS_DONE);
    ESP_LOGI(TAG, "%s: done. Unplug and connect the next board whenever you like.", t->name);
}

// While done: keep checking the connector. Board gone -> back to searching (blue);
// a different board plugged in -> handle it.
static void watch(target_t *t)
{
    TickType_t now = xTaskGetTickCount();
    if (now - t->last_watch < pdMS_TO_TICKS(1000))
        return;
    t->last_watch = now;
    uint32_t uid[3] = {0};
    if (swd_connect(&t->swd) != SWD_OK) {
        if (++t->lost >= 2) {
            ESP_LOGI(TAG, "%s: board disconnected, waiting for the next one", t->name);
            set_state(t, TS_SEARCH);
        }
        return;
    }
    t->lost = 0;
    if (t->have_done && swd_mem_read_block(&t->swd, 0x1FFFF7ACu, uid, 3) == SWD_OK &&
        memcmp(uid, t->done_uid, sizeof uid) != 0) {
        char u[25];
        uid_str(uid, u);
        ESP_LOGI(TAG, "%s: a different board is connected (uid %s)", t->name, u);
        set_state(t, TS_SEARCH);
    }
}

// Read-only: does the board's flash already hold exactly this image? (avoids re-flashing on every reboot)
static int image_matches(target_t *t)
{
    uint32_t buf[64];
    for (size_t off = 0; off < t->len; off += sizeof buf) {
        size_t n = t->len - off < sizeof buf ? t->len - off : sizeof buf;
        if (swd_mem_read_block(&t->swd, 0x08000000u + off, buf, (n + 3) / 4) != SWD_OK)
            return 0; // unreadable (e.g. read-protected): treat as different
        if (memcmp(buf, t->image + off, n) != 0)
            return 0;
    }
    return 1;
}

static void attempt(target_t *t)
{
    if (swd_connect(&t->swd) != SWD_OK) {
        if (t->state != TS_SEARCH)
            set_state(t, TS_SEARCH);
        log_search(t);
        return;
    }
    uint32_t uid[3] = {0};
    int uid_ok = swd_mem_read_block(&t->swd, 0x1FFFF7ACu, uid, 3) == SWD_OK;
    char u[25];
    uid_str(uid, u);
    if (uid_ok && t->have_done && memcmp(uid, t->done_uid, sizeof uid) == 0) {
        ESP_LOGI(TAG, "%s: board %s was already handled, nothing to do", t->name, u);
        finish(t, uid, 1);
        return;
    }
    ESP_LOGI(TAG, "%s: controller found (DPIDR=0x%08x uid=%s)", t->name, (unsigned)t->swd.dpidr, uid_ok ? u : "unreadable");

    if (!t->image_ok) {
        ESP_LOGW(TAG, "%s: no valid firmware image embedded: read-only backup, nothing is written", t->name);
        if (probe_and_dump(t) == 0) {
            finish(t, uid, uid_ok);
        } else {
            ESP_LOGE(TAG, "%s: reading the board failed, will retry", t->name);
            show_error(t);
        }
        return;
    }

#ifdef FLASH_ONLY_UID
    if (!uid_ok || strcmp(u, FLASH_ONLY_UID) != 0) {
        ESP_LOGE(TAG, "%s: uid %s is not the board this image is for (%s): refusing to write", t->name,
                 uid_ok ? u : "unreadable", FLASH_ONLY_UID);
        show_error(t);
        return;
    }
#endif
    if (image_matches(t)) {
        ESP_LOGI(TAG, "%s: board already has this firmware (%u bytes match), not rewriting", t->name, (unsigned)t->len);
        finish(t, uid, uid_ok);
        return;
    }

    gd32_cb_t cb = {.installing = on_installing, .user = t, .name = t->name};
    gd32_result_t r = gd32_flash(&t->swd, t->image, t->len, ALLOW_UNLOCK, &cb);

    switch (r) {
    case GD32_OK:
        ESP_LOGI(TAG, "%s: programmed and verified", t->name);
        t->unlock_tries = 0;
        // Re-read the UID now that read protection is surely off.
        if (swd_connect(&t->swd) == SWD_OK)
            uid_ok = swd_mem_read_block(&t->swd, 0x1FFFF7ACu, uid, 3) == SWD_OK;
        finish(t, uid, uid_ok);
        break;
    case GD32_NO_TARGET:
        set_state(t, TS_SEARCH);
        log_search(t);
        break;
    case GD32_UNLOCKED_RETRY:
        if (++t->unlock_tries > MAX_UNLOCK_TRIES) {
            ESP_LOGE(TAG, "%s: still read-protected after %d unlock attempts", t->name, MAX_UNLOCK_TRIES);
            t->unlock_tries = 0;
            show_error(t);
        } else {
            ESP_LOGI(TAG, "%s: protection removed, reconnecting", t->name);
        }
        break;
    case GD32_WRONG_CORE:
    case GD32_ERROR:
    default:
        ESP_LOGE(TAG, "%s: failed, will retry", t->name);
        show_error(t);
        break;
    }
}

// ---- Pin auto-detect ----------------------------------------------------------
// Header GPIOs of the ESP32-C6-DevKitC-1 that are safe to wiggle (no USB 12/13, LED 8,
// strapping 9/15, flash 24-30).
static const int SCAN_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23};
#define N_SCAN ((int)(sizeof SCAN_PINS / sizeof SCAN_PINS[0]))

static void release_pin(int g)
{
    gpio_reset_pin(g); // input, pull-up per IDF default -> then remove pulls
    gpio_set_direction(g, GPIO_MODE_INPUT);
    gpio_set_pull_mode(g, GPIO_FLOATING);
}

static int read_with_pull(int g, gpio_pull_mode_t m)
{
    gpio_set_direction(g, GPIO_MODE_INPUT);
    gpio_set_pull_mode(g, m);
    esp_rom_delay_us(300);
    return gpio_get_level(g);
}

// Try every ordered (SWCLK, SWDIO) pair. Fills up to `max` hits; returns how many found.
static int pin_scan(int (*hits)[2], int max)
{
    int stuck_low[N_SCAN];
    char line[256];
    int n = 0, pos = 0;
    pos += snprintf(line + pos, sizeof line - pos, "passive (pull-down/pull-up):");
    for (int i = 0; i < N_SCAN; i++) {
        int g = SCAN_PINS[i];
        int pd = read_with_pull(g, GPIO_PULLDOWN_ONLY), pu = read_with_pull(g, GPIO_PULLUP_ONLY);
        stuck_low[i] = !pd && !pu; // held low (GND, or a target's SWCLK pull-down): can't be SWDIO
        pos += snprintf(line + pos, sizeof line - pos, " G%d=%d%d", g, pd, pu);
        release_pin(g);
    }
    ESP_LOGI(TAG, "scan %s  (01=floating 11=held high 00=held low)", line);

    // Bridge test: drive one pin, see whether any other pin follows it (solder blob / shared wire).
    int bridges = 0;
    for (int i = 0; i < N_SCAN; i++) {
        int a = SCAN_PINS[i];
        if (stuck_low[i]) continue;
        gpio_reset_pin(a);
        gpio_set_direction(a, GPIO_MODE_OUTPUT);
        for (int j = 0; j < N_SCAN; j++) {
            int b = SCAN_PINS[j];
            if (b == a) continue;
            gpio_set_level(a, 1);
            int hi = read_with_pull(b, GPIO_PULLDOWN_ONLY);
            gpio_set_level(a, 0);
            int lo = read_with_pull(b, GPIO_PULLUP_ONLY);
            if (hi && !lo && j > i) {
                ESP_LOGW(TAG, "scan: GPIO%d and GPIO%d are connected to each other (short or same wire)", a, b);
                bridges++;
            }
            release_pin(b);
        }
        release_pin(a);
    }
    if (!bridges) ESP_LOGI(TAG, "scan: no shorts between ESP pins");

    for (int i = 0; i < N_SCAN && n < max; i++) {
        for (int j = 0; j < N_SCAN && n < max; j++) {
            if (i == j || stuck_low[j]) continue; // SWDIO must not be held low
            int clk = SCAN_PINS[i], dio = SCAN_PINS[j];
            int used = 0;
            for (int k = 0; k < n; k++)
                used |= hits[k][0] == clk || hits[k][1] == clk || hits[k][0] == dio || hits[k][1] == dio;
            if (used) continue;
            esp_swd_pins_t p;
            swd_t s;
            esp_swd_pins_init(&p, clk, dio, -1);
            p.half_cycles *= 20; // ~50 kHz while hunting: tolerant of long/poor wires
            swd_init(&s, &ESP_SWD_IO, &p);
            int rc = swd_connect(&s);
            if (rc == SWD_OK) {
                uint32_t cpuid = 0;
                swd_mem_read32(&s, 0xE000ED00u, &cpuid);
                ESP_LOGI(TAG, "scan: FOUND controller with SWCLK=GPIO%d SWDIO=GPIO%d DPIDR=0x%08x CPUID=0x%08x", clk, dio,
                         (unsigned)s.dpidr, (unsigned)cpuid);
                hits[n][0] = clk;
                hits[n][1] = dio;
                n++;
            } else if (s.last_ack != 7) {
                ESP_LOGI(TAG, "scan: SWCLK=GPIO%d SWDIO=GPIO%d something answered (ACK=%d%d%d id=0x%08x)", clk, dio,
                         s.last_ack & 1, (s.last_ack >> 1) & 1, (s.last_ack >> 2) & 1, (unsigned)s.last_id);
            }
            release_pin(clk);
            release_pin(dio);
        }
    }
    if (!n) ESP_LOGW(TAG, "scan: no controller answered on any pin pair");
    return n;
}

static void retarget(target_t *t, int clk, int dio)
{
    if (t->swclk == clk && t->swdio == dio) return;
    ESP_LOGW(TAG, "%s: using auto-detected pins SWCLK=GPIO%d SWDIO=GPIO%d (config.h says %d/%d)", t->name, clk, dio,
             t->swclk, t->swdio);
    release_pin(t->swclk);
    release_pin(t->swdio);
    t->swclk = clk;
    t->swdio = dio;
    esp_swd_pins_init(&t->pins, clk, dio, t->nrst);
    swd_init(&t->swd, &ESP_SWD_IO, &t->pins);
}

static void autodetect(void)
{
    int hits[2][2];
    for (int i = 0; i < NUM_TARGETS; i++) {
        release_pin(T[i].swclk);
        release_pin(T[i].swdio);
    }
    int n = pin_scan(hits, 2);
    // Restore configured pins, then move targets onto what was found.
    for (int i = 0; i < NUM_TARGETS; i++) {
        esp_swd_pins_init(&T[i].pins, T[i].swclk, T[i].swdio, T[i].nrst);
        swd_init(&T[i].swd, &ESP_SWD_IO, &T[i].pins);
    }
    for (int k = 0; k < n && k < NUM_TARGETS; k++) {
        // Prefer the target whose configured pins match; otherwise fill in order.
        int idx = k;
        for (int i = 0; i < NUM_TARGETS; i++)
            if (T[i].swclk == hits[k][0] && T[i].swdio == hits[k][1]) idx = i;
        retarget(&T[idx], hits[k][0], hits[k][1]);
    }
}

// ---- Raw link diagnostic -------------------------------------------------------
// Bit-bangs one DPIDR read and prints exactly what came back on SWDIO, sampled both just
// before and just after each rising SWCLK edge, at several clock speeds.
static void diag_delay(uint32_t cyc)
{
    uint32_t st = esp_cpu_get_cycle_count();
    while ((uint32_t)(esp_cpu_get_cycle_count() - st) < cyc) {
    }
}

static int g_drive = GPIO_DRIVE_CAP_2;
static void raw_dpidr(int clk, int dio, uint32_t half, char *pre, char *post, int nbits)
{
    gpio_reset_pin(clk);
    gpio_reset_pin(dio);
    gpio_set_direction(clk, GPIO_MODE_OUTPUT);
    gpio_set_direction(dio, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_drive_capability(clk, g_drive);
    gpio_set_drive_capability(dio, g_drive);
    gpio_set_pull_mode(dio, GPIO_PULLUP_ONLY);
#define OUTBIT(b) do { gpio_set_level(clk, 0); gpio_set_level(dio, (b)); diag_delay(half); gpio_set_level(clk, 1); diag_delay(half); } while (0)
    for (int i = 0; i < 56; i++) OUTBIT(1);
    for (int i = 0; i < 16; i++) OUTBIT((0xE79E >> i) & 1);
    for (int i = 0; i < 56; i++) OUTBIT(1);
    for (int i = 0; i < 8; i++) OUTBIT(0);
    for (int i = 0; i < 8; i++) OUTBIT((0xA5 >> i) & 1); // DPIDR read request
    gpio_set_direction(dio, GPIO_MODE_INPUT);
    for (int i = 0; i < nbits; i++) { // turnaround, 3 ACK, 32 data, parity, ...
        gpio_set_level(clk, 0);
        diag_delay(half);
        pre[i] = '0' + gpio_get_level(dio);
        gpio_set_level(clk, 1);
        diag_delay(half / 2 ? half / 2 : 1);
        post[i] = '0' + gpio_get_level(dio);
        diag_delay(half - half / 2);
    }
    pre[nbits] = post[nbits] = 0;
    gpio_set_direction(dio, GPIO_MODE_INPUT_OUTPUT);
    for (int i = 0; i < 8; i++) OUTBIT(0);
#undef OUTBIT
}

static void __attribute__((unused)) link_diag(int clk, int dio)
{
    static const uint32_t half_ns[] = {500, 5000};
    char pre[48], post[48];
    ESP_LOGI(TAG, "diag SWCLK=GPIO%d SWDIO=GPIO%d. expect: T ACK=100 then DPIDR 0x0bf11477 LSB-first = 11101110001010001000111111010000 + parity 0",
             clk, dio);
    for (int d = 0; d < 4; d++)
    for (unsigned k = 0; k < sizeof half_ns / sizeof half_ns[0]; k++) {
        g_drive = d;
        uint32_t half = (uint32_t)((uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * half_ns[k] / 1000);
        for (int rep = 0; rep < 2; rep++) {
            raw_dpidr(clk, dio, half, pre, post, 40);
            ESP_LOGI(TAG, "diag drive=%d half=%5uns pre-edge %.1s %.3s %.32s %.1s | post-edge %.1s %.3s %.32s %.1s", d, (unsigned)half_ns[k],
                     pre, pre + 1, pre + 4, pre + 36, post, post + 1, post + 4, post + 36);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void app_main(void)
{
    led_init();
    led_selftest();
#if SWD_LINK_DIAG
    link_diag(TARGET_A_SWCLK, TARGET_A_SWDIO);
#endif

    const uint8_t *a = fw_a;
    size_t alen = fw_a_len;
    const uint8_t *b = fw_b;
    size_t blen = fw_b_len;
    if (blen == 0) { // no board-B specific image: reuse A's (same SLAVE_ID, see README)
        b = a;
        blen = alen;
        if (alen && NUM_TARGETS > 1)
            ESP_LOGW(TAG, "firmware/target_b.bin is empty: using target_a.bin for board B too");
    }

    struct { const char *n; int clk, dio, rst; const uint8_t *img; size_t len; } cfg[2] = {
        {TARGET_A_NAME, TARGET_A_SWCLK, TARGET_A_SWDIO, TARGET_A_NRST, a, alen},
        {TARGET_B_NAME, TARGET_B_SWCLK, TARGET_B_SWDIO, TARGET_B_NRST, b, blen},
    };

    for (int i = 0; i < NUM_TARGETS; i++) {
        target_t *t = &T[i];
        t->name = cfg[i].n;
        t->swclk = cfg[i].clk;
        t->swdio = cfg[i].dio;
        t->nrst = cfg[i].rst;
        t->image = cfg[i].img;
        t->len = cfg[i].len;
        t->state = TS_SEARCH;
        t->image_ok = check_image(t->name, t->image, t->len);
        esp_swd_pins_init(&t->pins, cfg[i].clk, cfg[i].dio, cfg[i].rst);
        swd_init(&t->swd, &ESP_SWD_IO, &t->pins);
        ESP_LOGI(TAG, "target %s: SWCLK=GPIO%d SWDIO=GPIO%d image=%u bytes", t->name, cfg[i].clk, cfg[i].dio,
                 (unsigned)t->len);
        int pd, pu;
        esp_swd_line_check(&t->pins, &pd, &pu);
        ESP_LOGI(TAG, "target %s: SWDIO reads %d with pull-down, %d with pull-up (floating = 0 then 1)", t->name, pd, pu);
    }
    refresh_led();

    int misses = 0;
    for (;;) {
        // Nobody found on the configured pins for a while: hunt for the wiring.
        int any_found = 0;
        for (int i = 0; i < NUM_TARGETS; i++)
            any_found |= T[i].state != TS_SEARCH || T[i].have_done;
        if (!any_found && (misses == 0 || misses % 30 == 0)) {
            int alive = 0;
            for (int i = 0; i < NUM_TARGETS; i++)
                alive |= swd_connect(&T[i].swd) == SWD_OK;
            if (!alive) autodetect();
        }
        misses++;
        for (int i = 0; i < NUM_TARGETS; i++) {
            if (T[i].state == TS_DONE) watch(&T[i]);
            if (T[i].state != TS_DONE) attempt(&T[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}
