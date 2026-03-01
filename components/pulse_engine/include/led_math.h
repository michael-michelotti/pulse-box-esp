#ifndef INC_LED_MATH_H_
#define INC_LED_MATH_H_

#include <stdint.h>
#include <stdbool.h>

/* Both unions assume arm, little endian memory format */
typedef union
{
	uint32_t raw;
	struct {
		uint8_t v;  // lowest address on little-endian arm
		uint8_t s;
		uint16_t h;
	};
} HSV_t;

typedef union
{
	uint32_t raw;
	struct {
		uint8_t b;  // lowest address on little-endian arm
		uint8_t g;
		uint8_t r;
		uint8_t w;
	};
} WRGB_t;

typedef struct
{
	WRGB_t entries[16];
	const char *name;
} Palette_t;

typedef struct {
	WRGB_t colors[8];
	uint8_t num_colors;
	const char *name;
} ColorSet_t;


WRGB_t hsv_to_rgb(HSV_t hsv);
WRGB_t hsv_to_rgb_yellow_corrected(HSV_t hsv);
HSV_t rgb_to_hsv(WRGB_t rgb);

WRGB_t blend_colors(WRGB_t c1, WRGB_t c2, uint8_t blend);
WRGB_t add_colors(WRGB_t c1, WRGB_t c2, bool preserve_ratio);
WRGB_t dim_color(WRGB_t c1, uint8_t dim);

uint8_t gamma_correct(uint8_t value);
void init_gamma_table(float gamma);

WRGB_t palette_color_at(const Palette_t *p, uint8_t index);
// WRGB_t set_color_at(const ColorSet_t* cs, uint8_t index);
void init_rainbow_palette(void);
void init_fire_palette(void);
void init_palettes(void);

extern Palette_t rainbow_palette;
extern Palette_t fire_palette;

#endif /* INC_LED_MATH_H_ */
