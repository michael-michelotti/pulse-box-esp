#ifndef WIFI_AUDIO_H
#define WIFI_AUDIO_H

#include "audio.h"

#define AUDIO_UDP_PORT          5000
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_FFT_SIZE          2048
#define AUDIO_SAMPLES_PER_PKT   512

typedef struct __attribute__((packed)) {
    uint16_t magic;    // 0x5042 ("PB")
    uint16_t seq;      // Sequence number for gap detection
} AudioPacketHeader_t;

void wifi_audio_init(void);   // Creates UDP task, calls audio_init()

#endif
