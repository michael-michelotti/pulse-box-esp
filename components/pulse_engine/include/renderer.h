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

#endif /* INC_RENDERER_H_ */
