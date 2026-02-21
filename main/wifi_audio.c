#include "wifi_audio.h"

#include <string.h>
#include <math.h>
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
static int16_t *front_buffer = audio_buffer_0;  // Read by audio_update
static int16_t *back_buffer  = audio_buffer_1;  // Written by UDP task
static int     back_buffer_pos = 0;
static volatile bool audio_buffer_ready = false;

// FFT working memory: complex interleaved (real, imag pairs) = 2 * N floats
static float fft_work_buf[AUDIO_FFT_SIZE * 2];
// Hann window precomputed
static float hann_window[AUDIO_FFT_SIZE];

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
            // ESP_LOGW(TAG, "Received %d bytes but bad magic: 0x%04X", len, hdr->magic);
            continue;
        }

        int payload_bytes = len - sizeof(AudioPacketHeader_t);
        int num_samples = payload_bytes / sizeof(int16_t);
        // ESP_LOGI(TAG, "UDP recv: %d bytes, seq=%d, %d samples", len, hdr->seq, num_samples);
        int16_t *samples = (int16_t *)(udp_rx_buf + sizeof(AudioPacketHeader_t));

        // Copy samples into back buffer
        int space = AUDIO_FFT_SIZE - back_buffer_pos;
        int to_copy = (num_samples < space) ? num_samples : space;
        memcpy(&back_buffer[back_buffer_pos], samples, to_copy * sizeof(int16_t));
        back_buffer_pos += to_copy;

        // When back buffer is full, signal ready
        if (back_buffer_pos >= AUDIO_FFT_SIZE) {
            audio_buffer_ready = true;
            back_buffer_pos = 0;
            // ESP_LOGI(TAG, "Audio buffer ready (seq=%d, %d samples)", hdr->seq, AUDIO_FFT_SIZE);
        }
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
    // Swap buffers if new data is ready
    if (audio_buffer_ready) {
        int16_t *tmp = front_buffer;
        front_buffer = back_buffer;
        back_buffer = tmp;
        audio_buffer_ready = false;
    }

    // Apply Hann window and pack into complex format (real, imag pairs)
    // Keep raw int16 scale so FFT magnitudes match what effects expect from STM32
    for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
        fft_work_buf[i * 2]     = (float)front_buffer[i] * hann_window[i];
        fft_work_buf[i * 2 + 1] = 0.0f;  // Imaginary = 0 for real input
    }

    // Run FFT (complex radix-2, then bit-reversal to reorder bins)
    dsps_fft2r_fc32(fft_work_buf, AUDIO_FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_work_buf, AUDIO_FFT_SIZE);

    // Extract bass magnitude from bins 1-4
    // Each bin = sample_rate / fft_size = 48000/2048 ≈ 23.4 Hz
    // Bins 1-4 ≈ 23-94 Hz (sub-bass to bass)
    float bass_mag = 0.0f;
    for (int i = 1; i <= 4; i++) {
        float real = fft_work_buf[i * 2];
        float imag = fft_work_buf[i * 2 + 1];
        bass_mag += sqrtf(real * real + imag * imag);
    }

    // Convert to dB scale, then map to 0-255 range
    // Raw int16 FFT magnitudes: bass bins sum can reach ~100k at full volume
    // 20*log10(100000) ≈ 100 dB, so threshold at 60 dB, scale over 40 dB range
    float db = 20.0f * log10f(bass_mag + 1.0f);
    float scaled = fminf(fmaxf((db - 60.0f) * (255.0f / 40.0f), 0.0f), 255.0f);

    state->bass_magnitude = scaled;
    state->fft_bins = fft_work_buf;
    state->num_bins = AUDIO_FFT_SIZE / 2;
}

void wifi_audio_init(void)
{
    audio_init();
    xTaskCreatePinnedToCore(udp_audio_task, "udp_audio", 4096, NULL, 5, NULL, 1);
}
