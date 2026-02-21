#include "canvas.h"


void canvas_init(Canvas_t *canvas) {
	canvas->num_pixels = 64;  // hard code to 8 right now to prevent overheat
	canvas->num_panels = 1;
	canvas->min_x = 0;
	canvas->max_x = 7;
	canvas->min_y = 0;
	canvas->max_y = 7;

	canvas->panels[0] = (PanelInfo_t){
		.panel_id = 0,
		.center_x = 3.5f,
		.center_y = 3.5f,
		.mode = PANEL_MODE_GLOBAL
    };

	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			int led;
			/* 8x8 grid will zig-zag, need to change direction every row */
			if (y % 2 == 0) {
				led = y * 8 + x;        /* even rows: left to right */
			} else {
				led = y * 8 + (7 - x);  /* odd rows: right to left */
			}
			canvas->pixels[y * 8 + x] = (PixelMapping_t){
				.x = x,
				.y = y,
				.led_index = led,
				.panel_id = 0
			};
		}
	}
}
