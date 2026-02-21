#include "wifi_audio.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_dsp.h"

static const char *TAG = "wifi_audio";

#define AUDIO_MAGIC 0x5042

// Ping-pong PCM buffers (2048 samples each, 16-bit mono)
static int16_t audio_buffer_0[AUDIO_FFT_SIZE];
static int16_t audio_buffer_1[AUDIO_FFT_SIZE];
static int16_t *front_buffer = audio_buffer_0;  // Read by FFT task
static int16_t *back_buffer  = audio_buffer_1;  // Written by UDP task
static int     back_buffer_pos = 0;

// FFT working memory: complex interleaved (real, imag pairs) = 2 * N floats
static float fft_work_buf[AUDIO_FFT_SIZE * 2];
// Hann window precomputed
static float hann_window[AUDIO_FFT_SIZE];

// Double-buffered AudioState: FFT task writes back, render task reads front
static float fft_bins_0[AUDIO_FFT_SIZE * 2];
static float fft_bins_1[AUDIO_FFT_SIZE * 2];
static AudioState_t audio_state_0 = { .fft_bins = fft_bins_0, .num_bins = 0 };
static AudioState_t audio_state_1 = { .fft_bins = fft_bins_1, .num_bins = 0 };
static AudioState_t *audio_front = &audio_state_0;  // Read by render task
static AudioState_t *audio_back  = &audio_state_1;  // Written by FFT task

// Cross-core spinlock protecting audio_front pointer swap
static portMUX_TYPE audio_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Task handle for FFT task (UDP task notifies it)
static TaskHandle_t fft_task_handle = NULL;

// Receive buffer: header + max samples per packet
static uint8_t udp_rx_buf[sizeof(AudioPacketHeader_t) + (AUDIO_SAMPLES_PER_PKT * sizeof(int16_t))];

static void udp_audio_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP socket: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP audio listener started on port %d", AUDIO_UDP_PORT);

    while (1) {
        int len = recvfrom(sock, udp_rx_buf, sizeof(udp_rx_buf), 0, NULL, NULL);
        if (len < (int)sizeof(AudioPacketHeader_t)) {
            continue;
        }

        AudioPacketHeader_t *hdr = (AudioPacketHeader_t *)udp_rx_buf;
        if (hdr->magic != AUDIO_MAGIC) {
            continue;
        }

        int payload_bytes = len - sizeof(AudioPacketHeader_t);
        int num_samples = payload_bytes / sizeof(int16_t);
        int16_t *samples = (int16_t *)(udp_rx_buf + sizeof(AudioPacketHeader_t));

        // Copy samples into back buffer
        int space = AUDIO_FFT_SIZE - back_buffer_pos;
        int to_copy = (num_samples < space) ? num_samples : space;
        memcpy(&back_buffer[back_buffer_pos], samples, to_copy * sizeof(int16_t));
        back_buffer_pos += to_copy;

        // When back buffer is full, notify FFT task
        if (back_buffer_pos >= AUDIO_FFT_SIZE) {
            xTaskNotifyGive(fft_task_handle);
            back_buffer_pos = 0;
        }
    }
}

static void audio_fft_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio FFT task started on core %d", xPortGetCoreID());

    while (1) {
        // Wait for UDP task to signal a full buffer
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Swap PCM ping-pong buffers so UDP task can fill the other one
        int16_t *tmp = front_buffer;
        front_buffer = back_buffer;
        back_buffer = tmp;

        // Apply Hann window and pack into complex format (real, imag pairs)
        // Keep raw int16 scale so FFT magnitudes match what effects expect from STM32
        for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
            fft_work_buf[i * 2]     = (float)front_buffer[i] * hann_window[i];
            fft_work_buf[i * 2 + 1] = 0.0f;
        }

        // Run FFT
        dsps_fft2r_fc32(fft_work_buf, AUDIO_FFT_SIZE);
        dsps_bit_rev2r_fc32(fft_work_buf, AUDIO_FFT_SIZE);

        // Write FFT results into back AudioState
        memcpy(audio_back->fft_bins, fft_work_buf, AUDIO_FFT_SIZE * 2 * sizeof(float));
        audio_back->num_bins = AUDIO_FFT_SIZE / 2;

        // Swap front/back AudioState pointers under spinlock
        taskENTER_CRITICAL(&audio_spinlock);
        AudioState_t *swap = audio_front;
        audio_front = audio_back;
        audio_back = swap;
        taskEXIT_CRITICAL(&audio_spinlock);
    }
}

void audio_init(void)
{
    // Initialize ESP-DSP FFT tables for N-point radix-2
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, AUDIO_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Precompute Hann window
    dsps_wind_hann_f32(hann_window, AUDIO_FFT_SIZE);

    ESP_LOGI(TAG, "Audio FFT initialized (%d-point)", AUDIO_FFT_SIZE);
}

void audio_update(AudioState_t *state)
{
    // Copy the latest FFT results from the front buffer under spinlock
    taskENTER_CRITICAL(&audio_spinlock);
    state->fft_bins = audio_front->fft_bins;
    state->num_bins = audio_front->num_bins;
    taskEXIT_CRITICAL(&audio_spinlock);
}

void wifi_audio_init(void)
{
    audio_init();
    xTaskCreatePinnedToCore(audio_fft_task, "audio_fft", 8192, NULL, 4, &fft_task_handle, 1);
    xTaskCreatePinnedToCore(udp_audio_task, "udp_audio", 4096, NULL, 5, NULL, 1);
}
