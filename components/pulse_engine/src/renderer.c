#include "renderer.h"
#include "led_math.h"
#include <string.h>


static uint8_t framebuffer[MAX_PIXELS * 3];

void renderer_init(void) {
	init_gamma_table(DEFAULT_GAMMA);
	init_palettes();
}

void renderer_render_frame(const Canvas_t *canvas, const FrameState_t *frame,
		const Effect_t *effect, const EffectParams_t *params,
		const Mapping_t *mapping, const LedDriver_t *driver) {

	memset(framebuffer, 0, canvas->num_pixels * 3);
	effect->compute(canvas, frame, params, mapping, framebuffer);

	/* Scale for global brightness parameter */
	for (int i = 0; i < canvas->num_pixels * 3; i++) {
		framebuffer[i] = (framebuffer[i] * params->brightness) / 100;
	}

	/* Gamma correction */
	for (int i = 0; i < canvas->num_pixels; i++) {
		int idx = canvas->pixels[i].led_index * 3;
		framebuffer[idx + 0] = gamma_correct(framebuffer[idx + 0]);
		framebuffer[idx + 1] = gamma_correct(framebuffer[idx + 1]);
		framebuffer[idx + 2] = gamma_correct(framebuffer[idx + 2]);
	}

	driver->send_frame(framebuffer, canvas->num_pixels);
}
