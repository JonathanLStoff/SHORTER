#include "led.h"

#include "config.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static volatile led_mode_t s_mode = LED_OFF;

static void show(uint8_t r, uint8_t g, uint8_t b)
{
    // scale by brightness; WS2812 wants GRB order
    uint8_t px[3] = {
        (uint8_t)(g * LED_BRIGHTNESS / 255),
        (uint8_t)(r * LED_BRIGHTNESS / 255),
        (uint8_t)(b * LED_BRIGHTNESS / 255),
    };
    rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(s_chan, s_enc, px, sizeof px, &tc);
    rmt_tx_wait_all_done(s_chan, 100);
}

static void led_task(void *arg)
{
    (void)arg;
    int phase = 0;
    for (;;) {
        switch (s_mode) {
        case LED_SEARCH_BLUE:
            phase ^= 1;
            if (phase) show(0, 0, 255); else show(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(400));
            break;
        case LED_INSTALL_YELLOW: show(255, 160, 0); vTaskDelay(pdMS_TO_TICKS(100)); break;
        case LED_DONE_GREEN: show(0, 255, 0); vTaskDelay(pdMS_TO_TICKS(100)); break;
        case LED_ERROR_RED: show(255, 0, 0); vTaskDelay(pdMS_TO_TICKS(100)); break;
        default: show(0, 0, 0); vTaskDelay(pdMS_TO_TICKS(100)); break;
        }
    }
}

void led_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 0.1 us ticks
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &s_chan));

    rmt_bytes_encoder_config_t bc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9}, // 0.3 us high / 0.9 us low
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3}, // 0.9 us high / 0.3 us low
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bc, &s_enc));
    ESP_ERROR_CHECK(rmt_enable(s_chan));
    xTaskCreate(led_task, "led", 3072, NULL, 5, NULL);
}

void led_set(led_mode_t mode) { s_mode = mode; }

void led_selftest(void)
{
    led_set(LED_ERROR_RED);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_set(LED_DONE_GREEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_set(LED_SEARCH_BLUE);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_set(LED_INSTALL_YELLOW);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_set(LED_OFF);
    vTaskDelay(pdMS_TO_TICKS(150));
}
