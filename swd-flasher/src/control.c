// Control mode: drives both wheels and both boards' LEDs through the MASTER's debug port.
// The slave is reached only through the master (master-slave cable). Full description: API.md.
//
// Safety rules:
//  - Disarmed at boot. Nothing drives a motor until you type `arm`.
//  - Armed, a command frame goes out every CONTROL_HEARTBEAT_MS. The firmware stops both wheels
//    when the heartbeat stops changing for 600 ms, so a dead ESP32 or a pulled wire stops them.
//  - CONTROL_CMD_HOLD_MS > 0 makes a wheel command expire unless repeated (0 = commands persist).
//  - LEDs are separate from motion: `led` never arms anything.
//
// Console (type `help`):
//   arm | disarm | stop | st | off
//   s <master> [slave]           wheel commands -1000..1000, positive = forward. One value = both
//   led <master|slave|both> <green|orange|red|upper|lower|all> on|off
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mailbox.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "control";

static swd_t *s_swd;

// owned by the control loop
static int s_attached;
static uint32_t s_hb;
static TickType_t s_next_wait_log;

// set by the console task
static volatile int s_armed, s_disarm_req, s_status_req, s_led_dirty, s_off_req;
static volatile int32_t s_cmd_master, s_cmd_slave;
static volatile TickType_t s_last_cmd;
static volatile uint32_t s_led_master, s_led_slave;

static const struct { const char *name; uint32_t bit; } LEDS[] = {
    {"green", MBX_LED_GREEN}, {"orange", MBX_LED_ORANGE}, {"red", MBX_LED_RED},
    {"upper", MBX_LED_UPPER}, {"lower", MBX_LED_LOWER}, {"all", MBX_LED_ALL},
};

// The primary console is UART0; the USB port is an output-only secondary console. Read the
// commands from the USB-serial-JTAG driver (UART0 too, for a wired UART console).
static void console_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_serial_jtag_driver_install(&cfg);
    }
}

static int console_getc(void)
{
    uint8_t ch;
    if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10)) == 1) return ch;
    return getchar();
}

static int32_t clamp_cmd(long v)
{
    if (v > CONTROL_MAX_SPEED) return CONTROL_MAX_SPEED;
    if (v < -CONTROL_MAX_SPEED) return -CONTROL_MAX_SPEED;
    return (int32_t)v;
}

static void print_help(void)
{
    ESP_LOGI(TAG, "arm | disarm | stop | st | off");
    ESP_LOGI(TAG, "s <master> [slave]      -%d..%d, positive = forward, one value drives both", CONTROL_MAX_SPEED,
             CONTROL_MAX_SPEED);
    ESP_LOGI(TAG, "led <master|slave|both> <green|orange|red|upper|lower|all> on|off");
}

static void handle_led(void)
{
    char *who = strtok(NULL, " \t");
    char *name = strtok(NULL, " \t");
    char *state = strtok(NULL, " \t");
    if (!who || !name || !state) { ESP_LOGW(TAG, "usage: led <master|slave|both> <name|all> on|off"); return; }
    int m = !strcmp(who, "master") || !strcmp(who, "both");
    int sl = !strcmp(who, "slave") || !strcmp(who, "both");
    if (!m && !sl) { ESP_LOGW(TAG, "board: master, slave or both"); return; }
    uint32_t bits = 0;
    for (size_t i = 0; i < sizeof LEDS / sizeof LEDS[0]; i++)
        if (!strcmp(name, LEDS[i].name)) bits = LEDS[i].bit;
    if (!bits) { ESP_LOGW(TAG, "LED: green orange red upper lower all"); return; }
    int on = !strcmp(state, "on");
    if (!on && strcmp(state, "off")) { ESP_LOGW(TAG, "use on or off"); return; }
    if (m) s_led_master = on ? (s_led_master | bits) : (s_led_master & ~bits);
    if (sl) s_led_slave = on ? (s_led_slave | bits) : (s_led_slave & ~bits);
    s_led_dirty = 1;
    ESP_LOGI(TAG, "LEDs master=0x%02lx slave=0x%02lx", (unsigned long)s_led_master, (unsigned long)s_led_slave);
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "help")) {
        print_help();
    } else if (!strcmp(cmd, "arm")) {
        s_armed = 1;
        ESP_LOGW(TAG, "ARMED. Motion enabled (wheel commands still 0)");
    } else if (!strcmp(cmd, "disarm")) {
        s_cmd_master = s_cmd_slave = 0;
        s_armed = 0;
        s_disarm_req = 1;
        ESP_LOGW(TAG, "DISARMED. Stopping both wheels");
    } else if (!strcmp(cmd, "s")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        if (!a) { ESP_LOGW(TAG, "usage: s <master> [slave]"); return; }
        s_cmd_master = clamp_cmd(strtol(a, NULL, 10));
        s_cmd_slave = b ? clamp_cmd(strtol(b, NULL, 10)) : s_cmd_master;
        s_last_cmd = xTaskGetTickCount();
        ESP_LOGI(TAG, "wheels master=%ld slave=%ld (%s)", (long)s_cmd_master, (long)s_cmd_slave,
                 s_armed ? "armed" : "NOT armed: type arm");
    } else if (!strcmp(cmd, "stop")) {
        s_cmd_master = s_cmd_slave = 0;
        ESP_LOGI(TAG, "stop (still armed)");
    } else if (!strcmp(cmd, "led")) {
        handle_led();
    } else if (!strcmp(cmd, "st")) {
        s_status_req = 1;
    } else if (!strcmp(cmd, "off")) {
        s_armed = 0;
        s_cmd_master = s_cmd_slave = 0;
        s_off_req = 1;
        ESP_LOGW(TAG, "switching both boards off (power button boots them again)");
    } else {
        ESP_LOGI(TAG, "unknown command '%s' (type help)", cmd);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[80];
    size_t n = 0;
    for (;;) {
        int c = console_getc();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = 0;
            handle_line(line);
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

static const char *hall_str(uint32_t h, char buf[4])
{
    if (h == MBX_HALL_UNKNOWN) return "?";
    buf[0] = (char)('0' + (h & 7));
    buf[1] = (h == 0 || h == 7) ? '!' : 0;   // 0 and 7 are impossible: sensor missing or broken
    buf[2] = 0;
    return buf;
}

static void log_status(const mbx_t *m)
{
    char hm[4], hs[4];
    ESP_LOGI(TAG, "%s%s%s%s hb=%lu echo=%lu enable=%lu cmd=%ld/%ld up=%lums Vbus=%.2fV",
             (m->fwState & MBX_FW_DRIVING) ? "DRIVING" : "", (m->fwState & MBX_FW_STOPPED) ? "STOPPED" : "",
             (m->fwState & MBX_FW_HB_SEEN) ? "" : " (no heartbeat yet)",
             (m->fwState & MBX_FW_SLAVE_LINK) ? " slave-link-ok" : " SLAVE-LINK-DOWN",
             (unsigned long)m->heartbeat, (unsigned long)m->echo, (unsigned long)m->enable, (long)m->cmdMaster,
             (long)m->cmdSlave, (unsigned long)m->uptimeMs, m->vBus);
    ESP_LOGI(TAG, "master: I=%.2fA speed=%.1f odom=%ld hall=%s leds=0x%02lx", m->currentMaster, m->speedMaster,
             (long)m->odomMaster, hall_str(m->hallMaster, hm), (unsigned long)m->ledMaster);
    ESP_LOGI(TAG, "slave:  I=%.2fA speed=%.1f odom=%ld hall=%s leds=0x%02lx replies=%lu", m->currentSlave,
             m->speedSlave, (long)m->odomSlave, hall_str(m->hallSlave, hs), (unsigned long)m->ledSlave,
             (unsigned long)m->slaveRx);
}

static void lost(const char *why)
{
    ESP_LOGE(TAG, "%s: disarmed. The firmware stops both wheels within %d ms", why, MBX_TIMEOUT_MS);
    s_attached = 0;
    s_armed = 0;
}

// One pass. Never blocks for long.
static void step(TickType_t now)
{
    mbx_t m;

    if (!s_attached) {
        if (swd_connect(s_swd) == SWD_OK && mbx_read(s_swd, &m) == SWD_OK && m.magic == MBX_MAGIC) {
            s_attached = 1;
            s_hb = m.heartbeat;
            s_armed = 0;
            s_led_dirty = 1;   // re-send the LEDs: a rebooted master starts with them off
            ESP_LOGI(TAG, "master mailbox found at 0x%08x, disarmed", (unsigned)MBX_ADDR);
        } else if (now >= s_next_wait_log) {
            ESP_LOGW(TAG, "no mailbox on the master. Flash the MASTER image and connect it");
            s_next_wait_log = now + pdMS_TO_TICKS(2000);
        }
        return;
    }

    if (mbx_read(s_swd, &m) != SWD_OK || m.magic != MBX_MAGIC) { lost("link lost"); return; }

    if (s_status_req) {
        s_status_req = 0;
        log_status(&m);
    }

    if (s_off_req) {
        s_off_req = 0;
        if (mbx_power_off(s_swd) != SWD_OK) { lost("power-off write failed"); return; }
        s_attached = 0;   // the master is going away
        return;
    }

    if (s_led_dirty) {
        if (mbx_write_led(s_swd, s_led_master, s_led_slave) != SWD_OK) { lost("LED write failed"); return; }
        s_led_dirty = 0;
    }

    if (s_disarm_req) {
        s_disarm_req = 0;
        if (mbx_write_command(s_swd, ++s_hb, 0, 0, 0) != SWD_OK) lost("write failed");
        return;
    }

    if (s_armed) {
#if CONTROL_CMD_HOLD_MS > 0
        if ((now - s_last_cmd) > pdMS_TO_TICKS(CONTROL_CMD_HOLD_MS) && (s_cmd_master || s_cmd_slave)) {
            s_cmd_master = s_cmd_slave = 0;
            ESP_LOGI(TAG, "command expired, wheels back to 0");
        }
#endif
        if (mbx_write_command(s_swd, ++s_hb, s_cmd_master, s_cmd_slave, 1) != SWD_OK) lost("write failed");
    }
}

void control_run(swd_t *master)
{
    s_swd = master;
    console_init();
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "control mode: disarmed. Type `help`. Master on SWD connector 1; slave via the master");

    for (;;) {
        step(xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(CONTROL_HEARTBEAT_MS));
    }
}

// ---- Raw bench mode (CONTROL_MODE 2) -------------------------------------------------------
// RAM peek/poke over SWD for bench tests driven from a host script (analysis/bench_slave.py).
// Never touches flash. One request at a time, executed by the SWD loop (one SWD user only).
//   peek <addr>                -> "OK peek <addr> <value>"      (32-bit read)
//   poke8|poke16|poke32 <addr> <value> -> "OK poke <addr>"
//   ping                       -> "OK ping <connected 0|1>"
// Errors answer "ERR <text>". Numbers in C syntax (0x.. or decimal).
static volatile int r_req;          // 0 idle, 1 pending, 2 done
static volatile int r_kind;         // 0 peek, 8/16/32 poke
static volatile uint32_t r_addr, r_val;
static volatile int r_rc;
static int r_connected;

static void raw_line(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "ping")) { printf("OK ping %d\n", r_connected); fflush(stdout); return; }
    int kind = !strcmp(cmd, "peek") ? 0 : !strcmp(cmd, "poke8") ? 8 : !strcmp(cmd, "poke16") ? 16
             : !strcmp(cmd, "poke32") ? 32 : -1;
    if (kind < 0) { printf("ERR unknown %s\n", cmd); fflush(stdout); return; }
    char *a = strtok(NULL, " \t"), *v = strtok(NULL, " \t");
    if (!a || (kind && !v)) { printf("ERR usage\n"); fflush(stdout); return; }
    r_addr = strtoul(a, NULL, 0);
    r_val = v ? strtoul(v, NULL, 0) : 0;
    if ((r_addr & 0xFFFF0000u) != 0x20000000u) { printf("ERR RAM only (0x2000xxxx)\n"); fflush(stdout); return; }
    r_kind = kind;
    r_req = 1;
    while (r_req != 2) vTaskDelay(1);
    if (r_rc != SWD_OK) printf("ERR swd %d\n", r_rc);
    else if (kind == 0) printf("OK peek 0x%08lx 0x%08lx\n", (unsigned long)r_addr, (unsigned long)r_val);
    else printf("OK poke 0x%08lx\n", (unsigned long)r_addr);
    fflush(stdout);
    r_req = 0;
}

static void raw_console_task(void *arg)
{
    (void)arg;
    char line[80];
    size_t n = 0;
    for (;;) {
        int c = console_getc();
        if (c == EOF) { vTaskDelay(1); continue; }
        if (c == '\r') continue;
        if (c == '\n') { line[n] = 0; raw_line(line); n = 0; }
        else if (n < sizeof(line) - 1) line[n++] = (char)c;
    }
}

void control_run_raw(swd_t *target)
{
    console_init();
    xTaskCreate(raw_console_task, "raw_console", 3072, NULL, 5, NULL);
    ESP_LOGW(TAG, "raw bench mode: RAM peek/poke over SWD connector 1 (flash is never written)");
    for (;;) {
        if (r_req == 1) {
            int rc = SWD_OK;
            if (!r_connected) rc = swd_connect(target);
            if (rc == SWD_OK) {
                r_connected = 1;
                uint32_t v = 0;
                if (r_kind == 0) { rc = swd_mem_read32(target, r_addr, &v); r_val = v; }
                else if (r_kind == 8) rc = swd_mem_write8(target, r_addr, (uint8_t)r_val);
                else if (r_kind == 16) rc = swd_mem_write16(target, r_addr, (uint16_t)r_val);
                else rc = swd_mem_write32(target, r_addr, r_val);
            }
            if (rc != SWD_OK) r_connected = 0;
            r_rc = rc;
            r_req = 2;
        }
        vTaskDelay(1);
    }
}
