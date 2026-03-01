#ifndef INC_RENDERER_H_
#define INC_RENDERER_H_

#include "canvas.h"
#include "effects.h"
#include "mappings.h"
#include "led_driver.h"

#define DEFAULT_GAMMA	2.2f


void renderer_init(void);
void renderer_render_frame(const Canvas_t *canvas, const FrameState_t *frame,
		const Effect_t *effect, const EffectParams_t *params,
		const Mapping_t *mapping, const LedDriver_t *driver);

/**
 * Copy the latest post-brightness, pre-gamma framebuffer into out,
 * remapped from physical LED order to logical row-major grid order.
 * Buffer must hold at least canvas->num_pixels * 3 bytes.
 */
void renderer_get_preview(uint8_t *out, const Canvas_t *canvas);

#endif /* INC_RENDERER_H_ */
