#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "canvas.h"
#include "effects.h"
#include "led_math.h"
#include "mappings.h"
#include "renderer.h"
#include "esp_led_driver.h"
#include "console.h"
#include "wifi_audio.h"

static const char *TAG = "pulsebox";

// WiFi credentials — replace with your network
#define WIFI_SSID     "ATTvVJZtdS"
#define WIFI_PASSWORD "exu4p?#%6m#7"

// Event group bit indicating we have an IP address
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_event_group;

// Same globals as STM32 main.c
Canvas_t canvas;
ColorSet_t default_color_set;
EffectParams_t effect_params;
const Effect_t *current_effect = &rainbow_effect;
const Mapping_t *current_mapping = &map_global;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA init complete, connecting to %s...", WIFI_SSID);

    // Wait for connection (block until IP acquired)
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi connected");
}

static void mdns_init_service(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("pulsebox"));
    ESP_ERROR_CHECK(mdns_instance_name_set("PulseBox LED Controller"));
    ESP_ERROR_CHECK(mdns_service_add("PulseBox Audio", "_pulsebox-audio", "_udp",
                                     AUDIO_UDP_PORT, NULL, 0));
    ESP_LOGI(TAG, "mDNS registered: pulsebox.local, audio service on UDP %d", AUDIO_UDP_PORT);
}

void app_main(void)
{
    // NVS is required by WiFi driver
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize color set with blue
    default_color_set.colors[0] = (WRGB_t){ .r = 0, .g = 0, .b = 255 };
    default_color_set.num_colors = 1;
    default_color_set.name = "default";

    // Initialize effect params
    effect_params.brightness = 10;
    effect_params.sensitivity = 50;
    effect_params.speed = 0;
    effect_params.palette = &rainbow_palette;
    effect_params.color_set = &default_color_set;

    // Initialize subsystems
    canvas_init(&canvas);
    esp32_ws2812b_driver.init();
    renderer_init();
    console_init();

    // Connect to WiFi and register mDNS
    wifi_init_sta();
    mdns_init_service();

    // Start UDP audio receiver + FFT on core 1
    wifi_audio_init();

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
