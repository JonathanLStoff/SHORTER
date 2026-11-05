#include "esp_swd_io.h"

#include "config.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void esp_swd_pins_init(esp_swd_pins_t *p, int swclk, int swdio, int nrst)
{
    p->swclk = swclk;
    p->swdio = swdio;
    p->nrst = nrst;
    uint64_t cycles = (uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ull * SWD_HALF_PERIOD_NS / 1000000000ull;
    p->half_cycles = cycles ? (uint32_t)cycles : 1;

    gpio_config_t clk = {
        .pin_bit_mask = 1ull << swclk,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&clk));
    gpio_set_level(swclk, 1);

    gpio_config_t dio = {
        .pin_bit_mask = 1ull << swdio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // keeps the line high (and "no target") when released
    };
    ESP_ERROR_CHECK(gpio_config(&dio));
    gpio_set_level(swdio, 1);
    gpio_set_drive_capability(swclk, (gpio_drive_cap_t)SWD_DRIVE_STRENGTH);
    gpio_set_drive_capability(swdio, (gpio_drive_cap_t)SWD_DRIVE_STRENGTH);

    if (nrst >= 0) {
        gpio_config_t rst = {
            .pin_bit_mask = 1ull << nrst,
            .mode = GPIO_MODE_OUTPUT_OD, // released = high-z, the board's own pull-up wins
        };
        ESP_ERROR_CHECK(gpio_config(&rst));
        gpio_set_level(nrst, 1);
    }
}

static void io_clk(void *c, int level) { gpio_set_level(((esp_swd_pins_t *)c)->swclk, level); }
static void io_dio_write(void *c, int level) { gpio_set_level(((esp_swd_pins_t *)c)->swdio, level); }
static int io_dio_read(void *c) { return gpio_get_level(((esp_swd_pins_t *)c)->swdio); }

static void io_dio_mode(void *c, int output)
{
    esp_swd_pins_t *p = c;
    // INPUT_OUTPUT mode keeps the pad readable either way; only the output driver changes.
    gpio_set_direction(p->swdio, output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

static void io_nrst(void *c, int level)
{
    esp_swd_pins_t *p = c;
    if (p->nrst >= 0)
        gpio_set_level(p->nrst, level);
}

static void io_delay(void *c)
{
    uint32_t start = esp_cpu_get_cycle_count();
    uint32_t n = ((esp_swd_pins_t *)c)->half_cycles;
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < n) {
    }
}

static void io_delay_ms(void *c, uint32_t ms)
{
    (void)c;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1); // at least one tick, so lower-priority tasks (IDLE / watchdog) get to run
}

const swd_io_t ESP_SWD_IO = {
    .clk = io_clk,
    .dio_write = io_dio_write,
    .dio_mode = io_dio_mode,
    .dio_read = io_dio_read,
    .nrst = io_nrst,
    .delay = io_delay,
    .delay_ms = io_delay_ms,
};

void esp_swd_line_check(const esp_swd_pins_t *p, int *with_pulldown, int *with_pullup)
{
    gpio_set_direction(p->swdio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(p->swdio, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(500);
    *with_pulldown = gpio_get_level(p->swdio);
    gpio_set_pull_mode(p->swdio, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(500);
    *with_pullup = gpio_get_level(p->swdio);
    gpio_set_direction(p->swdio, GPIO_MODE_INPUT_OUTPUT);
}
