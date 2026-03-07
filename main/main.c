#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"

#include "canvas.h"
#include "effects.h"
#include "led_math.h"
#include "mappings.h"
#include "renderer.h"
#include "esp_led_driver.h"
#include "console.h"
#include "wifi_audio.h"
#include "tcp_cmd_server.h"
#include "panel_bus.h"

static const char *TAG = "pulsebox";

#define NVS_NAMESPACE       "pulsebox"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASSWORD    "wifi_pass"

#define AP_SSID             "PulseBox-Setup"
#define AP_PASSWORD         "pulsebox123"
#define AP_CHANNEL          1
#define AP_MAX_CONN         2

#define STA_CONNECT_TIMEOUT_MS  15000

// Event group bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t wifi_event_group;
static int sta_retry_count = 0;
#define STA_MAX_RETRIES     5

// WiFi mode tracking — visible to console.c for the wifi command
bool wifi_is_ap_mode = false;

// Same globals as STM32 main.c
Canvas_t canvas;
ColorSet_t default_color_set;
EffectParams_t effect_params;
const Effect_t *current_effect = &rainbow_effect;
const Mapping_t *current_mapping = &map_global;

/* ---- NVS WiFi Credential Storage ---- */

bool wifi_creds_load(char *ssid, size_t ssid_size, char *password, size_t pass_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &pass_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    return true;
}

bool wifi_creds_save(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write SSID failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    return true;
}

/* ---- WiFi Event Handling ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (sta_retry_count < STA_MAX_RETRIES) {
            sta_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d...", sta_retry_count, STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "WiFi connection failed after %d retries", STA_MAX_RETRIES);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        sta_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- WiFi STA Mode ---- */

static bool wifi_init_sta(const char *ssid, const char *password)
{
    wifi_event_group = xEventGroupCreate();
    sta_retry_count = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA connecting to '%s'...", ssid);

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
        wifi_is_ap_mode = false;
        return true;
    }

    ESP_LOGW(TAG, "WiFi STA connection failed, will start AP mode");

    // Clean up STA before switching to AP
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    esp_netif_deinit();

    return false;
}

/* ---- WiFi AP Mode (fallback) ---- */

static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .password = AP_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_is_ap_mode = true;
    ESP_LOGI(TAG, "WiFi AP started: '%s' (open), connect and configure at 192.168.4.1:%d",
             AP_SSID, TCP_CMD_PORT);
}

/* ---- mDNS (STA mode only) ---- */

static void mdns_init_service(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("pulsebox"));
    ESP_ERROR_CHECK(mdns_instance_name_set("PulseBox LED Controller"));
    ESP_ERROR_CHECK(mdns_service_add("PulseBox Audio", "_pulsebox-audio", "_udp",
                                     AUDIO_UDP_PORT, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add("PulseBox Commands", "_pulsebox-cmd", "_tcp",
                                     TCP_CMD_PORT, NULL, 0));
    ESP_LOGI(TAG, "mDNS registered: pulsebox.local, audio UDP %d, commands TCP %d",
             AUDIO_UDP_PORT, TCP_CMD_PORT);
}

/* ---- Render Task ---- */

static void render_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    FrameState_t frame = {0};
    float prev_time = 0;
    int64_t last_frame_us = 0;
    int preview_counter = 0;

    extern volatile bool canvas_updating;

    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_frame_us >= 33333) { // ~30 FPS
            if (canvas_updating) {
                last_frame_us = now;
                vTaskDelay(1);
                continue;
            }

            frame.time = now / 1000000.0f;
            frame.dt = frame.time - prev_time;
            prev_time = frame.time;

            renderer_render_frame(&canvas, &frame, current_effect,
                                  &effect_params, current_mapping,
                                  &esp32_ws2812b_driver);

            /* Push live preview to desktop at ~1 Hz */
            if (++preview_counter >= 30) {
                preview_counter = 0;
                tcp_push_preview();
            }

            last_frame_us = now;
        }
        vTaskDelay(1); // Yield to idle task
    }
}

/* ---- App Entry Point ---- */

void app_main(void)
{
    // NVS is required by WiFi driver
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize color set with 3 default colors
    default_color_set.colors[0] = (WRGB_t){ .r = 0, .g = 0, .b = 255 };
    default_color_set.colors[1] = (WRGB_t){ .r = 255, .g = 0, .b = 0 };
    default_color_set.colors[2] = (WRGB_t){ .r = 0, .g = 255, .b = 0 };
    default_color_set.num_colors = 3;
    default_color_set.name = "default";

    // Initialize effect params
    effect_params.brightness = 10;
    effect_params.sensitivity = 50;
    effect_params.speed = 0.5;
    effect_params.palette = &rainbow_palette;
    effect_params.color_set = &default_color_set;

    // Initialize subsystems
    canvas_init(&canvas);      /* default single-panel; panel_bus_task may rebuild */
    esp32_ws2812b_driver.init();
    renderer_init();
    console_init();
    panel_bus_init();

    // Try loading WiFi credentials from NVS and connecting as STA
    char ssid[33] = {0};
    char password[65] = {0};
    bool sta_connected = false;

    if (wifi_creds_load(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Found WiFi credentials in NVS for '%s'", ssid);
        sta_connected = wifi_init_sta(ssid, password);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials in NVS");
    }

    if (sta_connected) {
        // STA mode — register mDNS and start audio receiver
        mdns_init_service();
        wifi_audio_init();
    } else {
        // AP mode — no mDNS or audio, just TCP commands for configuration
        wifi_init_ap();
    }

    ESP_LOGI(TAG, "PulseBox started (%s mode), creating tasks",
             wifi_is_ap_mode ? "AP" : "STA");

    // Create render task on core 0
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 5, NULL, 0);

    // Create console task on core 0
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 3, NULL, 0);

    // Create TCP command server task on core 0 (works in both AP and STA mode)
    xTaskCreatePinnedToCore(tcp_cmd_server_task, "tcp_cmd", 4096, NULL, 3, NULL, 0);

    // Create panel bus task on core 0 (discovery + hot-swap monitoring)
    xTaskCreatePinnedToCore(panel_bus_task, "panel_bus", 4096, NULL, 4, NULL, 0);

    // app_main returns — FreeRTOS scheduler keeps running the created tasks
}
