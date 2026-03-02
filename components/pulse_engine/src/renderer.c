#include "renderer.h"
#include "led_math.h"
#include <string.h>


static uint8_t framebuffer[MAX_PIXELS * 3];
static uint8_t preview_buf[MAX_PIXELS * 3];

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

	/* Snapshot post-brightness, pre-gamma for live preview */
	memcpy(preview_buf, framebuffer, canvas->num_pixels * 3);

	/* Gamma correction (table guarantees non-zero in → non-zero out) */
	if (!effect->skip_gamma) {
		for (int i = 0; i < canvas->num_pixels; i++) {
			int idx = canvas->pixels[i].led_index * 3;
			framebuffer[idx + 0] = gamma_correct(framebuffer[idx + 0]);
			framebuffer[idx + 1] = gamma_correct(framebuffer[idx + 1]);
			framebuffer[idx + 2] = gamma_correct(framebuffer[idx + 2]);
		}
	}

	driver->send_frame(framebuffer, canvas->num_pixels);
}

void renderer_get_preview(uint8_t *out, const Canvas_t *canvas) {
	/* Remap from physical LED order to logical row-major grid order */
	for (int i = 0; i < canvas->num_pixels; i++) {
		int phys = canvas->pixels[i].led_index * 3;
		int logical = i * 3;
		out[logical + 0] = preview_buf[phys + 0];
		out[logical + 1] = preview_buf[phys + 1];
		out[logical + 2] = preview_buf[phys + 2];
	}
}
