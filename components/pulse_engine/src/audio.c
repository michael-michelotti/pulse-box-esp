#include "audio.h"
#include <string.h>

/* Weak default implementations — overridden by platform-specific audio code */

static float dummy_bins[2048] = {0};

__attribute__((weak)) void audio_init(void)
{
}

__attribute__((weak)) void audio_update(AudioState_t *state)
{
    state->bass_magnitude = 0;
    state->fft_bins = dummy_bins;
    state->num_bins = 1024;
}
