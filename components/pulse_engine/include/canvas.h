#ifndef INC_CANVAS_H_
#define INC_CANVAS_H_

#include <stdint.h>

#define MAX_PIXELS 512	// supports up to 8 panels of 64 (8x8 LEDs)
#define MAX_PANELS 8

typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t led_index;
	uint8_t panel_id;
} PixelMapping_t;

typedef enum {
	PANEL_MODE_GLOBAL,
	PANEL_MODE_INDEPENDENT
} PanelMode_t;

typedef struct {
	uint8_t panel_id;
	float center_x;
	float center_y;
	PanelMode_t mode;
} PanelInfo_t;

typedef struct {
	uint16_t num_pixels;
	uint8_t num_panels;
	int16_t min_x, max_x;
	int16_t min_y, max_y;
	PanelInfo_t panels[MAX_PANELS];
	PixelMapping_t pixels[MAX_PIXELS];
} Canvas_t;

void canvas_init(Canvas_t *canvas);

#endif /* INC_CANVAS_H_ */
