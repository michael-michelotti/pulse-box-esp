#include "mappings.h"

static void global_transform(const Canvas_t *canvas, uint8_t panel_id,
		float *x, float *y) {
	(void)canvas;
	(void)panel_id;
}

const Mapping_t map_global = { .name = "global", .transform = global_transform };
