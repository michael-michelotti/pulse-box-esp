#ifndef INC_EFFECTS_H_
#define INC_EFFECTS_H_

#include <stdint.h>

#include "canvas.h"
#include "led_math.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
	uint8_t brightness;
	uint8_t sensitivity;
	float width;
	float speed;
	float direction;
	ColorSet_t *color_set;
	const Palette_t *palette;
} EffectParams_t;

typedef struct {
	float time;
	float dt;		// time since last frame
	void *context;	// optional, could be AudioState_t or NULL
} FrameState_t;

typedef struct Mapping Mapping_t;

typedef struct {
	const char *name;
	void (*compute)(const Canvas_t *canvas, const FrameState_t *frame,
			const EffectParams_t *params, const Mapping_t *mapping,
			uint8_t *framebuffer);
} Effect_t;

extern const Effect_t bass_pulse_effect;
extern const Effect_t rainbow_effect;
extern const Effect_t twinkle_effect;
extern const Effect_t solid_effect;
extern const Effect_t bass_splash_effect;
extern const Effect_t fire_effect;
extern const Effect_t breathe_effect;
extern const Effect_t wipe_effect;
extern const Effect_t spectrum_effect;
extern const Effect_t image_effect;

#endif /* INC_EFFECTS_H_ */
