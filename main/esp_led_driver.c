#include "esp_led_driver.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "led_driver";
static led_strip_handle_t strip = NULL;

static void esp_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_MAX_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz = 100ns per tick
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    led_strip_clear(strip);
    ESP_LOGI(TAG, "WS2812B strip initialized on GPIO %d, max %d LEDs",
             LED_STRIP_GPIO, LED_STRIP_MAX_LEDS);
}

static void esp_led_send_frame(const uint8_t *framebuffer, uint16_t num_pixels)
{
    for (int i = 0; i < num_pixels; i++) {
        led_strip_set_pixel(strip, i,
            framebuffer[i * 3 + 0],   // R
            framebuffer[i * 3 + 1],   // G
            framebuffer[i * 3 + 2]);  // B
    }
    led_strip_refresh(strip);
}

const LedDriver_t esp32_ws2812b_driver = {
    .name = "esp32_ws2812b",
    .init = esp_led_init,
    .send_frame = esp_led_send_frame,
};
