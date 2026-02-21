#ifndef INC_AUDIO_H_
#define INC_AUDIO_H_

#include <stdint.h>

typedef struct {
	float bass_magnitude;
	float *fft_bins;
	uint16_t num_bins;
} AudioState_t;


void audio_init(void);
void audio_update(AudioState_t *state);

#endif /* INC_AUDIO_H_ */
