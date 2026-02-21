#include "audio.h"
#include <string.h>

static float dummy_bins[2048] = {0};

void audio_init(void)
{
}

void audio_update(AudioState_t *state)
{
    state->bass_magnitude = 0;
    state->fft_bins = dummy_bins;
    state->num_bins = 1024;
}
