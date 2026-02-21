#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "canvas.h"
#include "effects.h"
#include "led_math.h"
#include "mappings.h"
#include "renderer.h"
#include "esp_led_driver.h"
#include "console.h"

static const char *TAG = "pulsebox";

// Same globals as STM32 main.c
Canvas_t canvas;
ColorSet_t default_color_set;
EffectParams_t effect_params;
const Effect_t *current_effect = &rainbow_effect;
const Mapping_t *current_mapping = &map_global;

void app_main(void)
{
    // Initialize color set with blue
    default_color_set.colors[0] = (WRGB_t){ .r = 0, .g = 0, .b = 255 };
    default_color_set.num_colors = 1;
    default_color_set.name = "default";

    // Initialize effect params
    effect_params.brightness = 10;
    effect_params.sensitivity = 50;
    effect_params.speed = 0.5f;
    effect_params.palette = &rainbow_palette;
    effect_params.color_set = &default_color_set;

    // Initialize subsystems
    canvas_init(&canvas);
    esp32_ws2812b_driver.init();
    renderer_init();
    console_init();

    ESP_LOGI(TAG, "PulseBox started, running rainbow effect");

    FrameState_t frame = {0};
    float prev_time = 0;
    int64_t last_frame_us = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        console_process();
        if (now - last_frame_us >= 33333) { // ~30 FPS
            frame.time = now / 1000000.0f;
            frame.dt = frame.time - prev_time;
            prev_time = frame.time;

            renderer_render_frame(&canvas, &frame, current_effect,
                                  &effect_params, current_mapping,
                                  &esp32_ws2812b_driver);

            last_frame_us = now;
        }
        vTaskDelay(1); // Yield to idle task
    }
}
