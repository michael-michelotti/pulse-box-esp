#ifndef INC_MAPPINGS_H_
#define INC_MAPPINGS_H_

#include <stdint.h>

#include "canvas.h"

typedef struct Mapping {
	const char *name;
	void (*transform)(const Canvas_t *canvas, uint8_t panel_id,
			float *x, float *y);
} Mapping_t;

extern const Mapping_t map_global;

#endif /* INC_MAPPINGS_H_ */
