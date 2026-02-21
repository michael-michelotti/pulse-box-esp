#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "led_math.h"


const uint32_t TWO_CHANNEL_MASK = 0x00FF00FF;
uint8_t gammaT[256];
uint8_t inv_gammaT[256];
Palette_t rainbow_palette;
Palette_t fire_palette;


/* receive two colors in WRGB, create mix of ~(blend/255)*100 % c2, rest c1  */
WRGB_t blend_colors(WRGB_t c1, WRGB_t c2, uint8_t blend) {
	uint32_t rb1 = c1.raw & TWO_CHANNEL_MASK;
	uint32_t rb2 = c2.raw & TWO_CHANNEL_MASK;
	uint32_t wg1 = (c1.raw >> 8) & TWO_CHANNEL_MASK;
	uint32_t wg2 = (c2.raw >> 8) & TWO_CHANNEL_MASK;

	uint32_t rb3 = (((rb1 << 8) | rb2) + (rb2 * blend) - (rb1 * blend)) >> 8 & TWO_CHANNEL_MASK;
	uint32_t wg3 = (((wg1 << 8) | wg2) + (wg2 * blend) - (wg1 * blend)) & ~TWO_CHANNEL_MASK;

	return (WRGB_t) { .raw = rb3 | wg3 };
}

void init_rainbow_palette(void) {
	for (int i = 0; i < 16; i++) {
		HSV_t hsv = {
				.h = (uint16_t)(i * (65536 / 16)),
				.s = 255,
				.v = 255
		};
		rainbow_palette.entries[i] = hsv_to_rgb(hsv);
	}
	rainbow_palette.name = "rainbow";
}

void init_fire_palette(void) {
	WRGB_t entries[16] = {
    		{ .r =   0, .g =   0, .b =   0 },  //  0 - black
			{ .r =  10, .g =   0, .b =   0 },  //  1 - near black
			{ .r =  30, .g =   0, .b =   0 },  //  2 - very dark red
			{ .r =  80, .g =   0, .b =   0 },  //  3 - dark red
			{ .r = 150, .g =   0, .b =   0 },  //  4 - red
			{ .r = 220, .g =  20, .b =   0 },  //  5 - red-orange
			{ .r = 255, .g =  60, .b =   0 },  //  6 - orange
			{ .r = 255, .g = 120, .b =   0 },  //  7 - orange
			{ .r = 255, .g = 180, .b =   0 },  //  8 - yellow-orange
			{ .r = 255, .g = 220, .b =   0 },  //  9 - yellow
			{ .r = 255, .g = 255, .b =  20 },  // 10 - bright yellow
			{ .r = 255, .g = 255, .b = 100 },  // 11 - pale yellow
			{ .r = 255, .g = 255, .b = 180 },  // 12 - near white
			{ .r = 255, .g = 255, .b = 220 },  // 13 - white-hot
			{ .r = 255, .g = 255, .b = 255 },  // 14 - white
			{ .r = 255, .g = 255, .b = 255 },  // 15 - white
    };
    for (int i = 0; i < 16; i++) {
    	fire_palette.entries[i] = entries[i];
    }
    fire_palette.name = "fire";
}

void init_palettes(void) {
	init_rainbow_palette();
	init_fire_palette();
}

WRGB_t palette_color_at(const Palette_t *p, uint8_t index) {
	uint8_t entry = index >> 4;
	uint8_t blend = (index & 0x0F) << 4;
	uint8_t next = (entry >= 15) ? 15 : entry + 1;
	return blend_colors(p->entries[entry], p->entries[next], blend);
}

WRGB_t hsv_to_rgb_base(HSV_t hsv, bool correct_yellow) {
	uint32_t remainder, sector, p, q, t;
	uint32_t h = (uint32_t) hsv.h;
	uint32_t s = (uint32_t) hsv.s;
	uint32_t v = (uint32_t) hsv.v;

	if (s == 0) {
		return (WRGB_t){ .raw = (v << 16 | v << 8 | v) };
	}

	sector    = h / 10923;
	remainder = (h - (sector * 10923)) * 6;
	p = (v * (255 - s)) >> 8;
	q = (v * (255 - ((s * remainder) >> 16))) >> 8;
	t = (v * (255 - ((s * (65535 - remainder)) >> 16))) >> 8;

	uint32_t r, g, b;
	switch (sector) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
    }

	if (correct_yellow) {
	    /* Yellow correction: in standard HSV, pure yellow hits r=255 g=255
	     * simultaneously, which looks white/washed out in a rainbow sweep.
	     * Cap r+g at 341 so yellow stays distinct — same approach as
	     * FastLED's hsv2rgb_rainbow, which outputs r=171 g=170 at pure yellow. */
	    if (sector <= 1) {
	        uint32_t rg = r + g;
	        if (rg > 341) {
	            r = (r * 341) / rg;
	            g = (g * 341) / rg;
	        }
	    }
	}

    return (WRGB_t){ .raw = (r << 16) | (g << 8) | b };
}

WRGB_t hsv_to_rgb(HSV_t hsv) {
	return hsv_to_rgb_base(hsv, false);
}


/* Yellow has a higher inherent brightness than any other color;
 * 'pure' yellow is perceived to be 93% as bright as white.
 * In order to make yellow appear the correct relative brightness,
 * it has to be rendered brighter than all other colors. */
WRGB_t hsv_to_rgb_yellow_corrected(HSV_t hsv) {
	return hsv_to_rgb_base(hsv, true);
}


HSV_t rgb_to_hsv(WRGB_t rgb) {
	HSV_t hsv = { .raw = 0 };
	uint32_t r = (uint32_t) rgb.r;
	uint32_t g = (uint32_t) rgb.g;
	uint32_t b = (uint32_t) rgb.b;
	uint32_t min, max, delta;
	int32_t h_val;

	max = r;
	if (g > max) max = g;
	if (b > max) max = b;
	if (max == 0) return hsv;

	min = r;
	if (g < min) min = g;
	if (b < min) min = b;

	delta = max - min;
	hsv.v = max;
	hsv.s = (255 * delta) / max;
	if (hsv.s == 0) return hsv;

	if (max == r) {
		h_val = (10923 * (int32_t)(g - b)) / (int32_t)delta;
	}
	else if (max == g) {
		h_val = (10923 * (int32_t)(b - r)) / (int32_t)delta;
	}
	else {
		h_val = (10923 * (int32_t)(r - g)) / (int32_t)delta;
	}

	if (h_val < 0) h_val += 65536;
	hsv.h = (uint16_t)h_val;

	return hsv;
}


WRGB_t add_colors(WRGB_t c1, WRGB_t c2, bool preserve_ratio) {
	uint32_t rb = (c1.raw & TWO_CHANNEL_MASK) + (c2.raw & TWO_CHANNEL_MASK);
	uint32_t wg = ((c1.raw>>8) & TWO_CHANNEL_MASK) + ((c2.raw>>8) & TWO_CHANNEL_MASK);

	if (preserve_ratio) {
		/* There was overflow if 9th bit is set for any color channel */
		uint32_t overflow_bits = (rb | wg) & 0x01000100;
		if (overflow_bits) {
			uint32_t r = rb >> 16;
			uint32_t b = rb & 0xFFFF;
			uint32_t w = wg >> 16;
			uint32_t g = wg & 0xFFFF;
			uint32_t max = r;
			if (b > max) max = b;
			if (w > max) max = w;
			if (g > max) max = g;
			uint32_t scale = ((uint32_t)255<<8) / max;
			rb = ((rb * scale) >> 8) & TWO_CHANNEL_MASK;
			wg = (wg * scale) & ~TWO_CHANNEL_MASK;
		}
		else {
			wg <<= 8;  // If there was no overflow in any of WRGB, simply put wg back
		}
	}
	else {
		/* If there was any overflow (9th bit set), simply clip any overflowed fields to 0xFF */
		rb |= ((rb & 0x01000100) - ((rb >> 8) & 0x00010001)) & 0x00FF00FF;
		wg |= ((wg & 0x01000100) - ((wg >> 8) & 0x00010001)) & 0x00FF00FF;
		wg <<= 8;
	}
	return (WRGB_t) { .raw = rb | wg };
}

/* return c1 with the same color ratio, but all colors reduced by dim/255 percent */
WRGB_t dim_color(WRGB_t c1, uint8_t dim) {
	dim += 1;
	uint32_t rb = (((c1.raw & TWO_CHANNEL_MASK) * dim) >> 8) & TWO_CHANNEL_MASK;
	uint32_t wg = (((c1.raw >> 8) & TWO_CHANNEL_MASK) * dim) & ~TWO_CHANNEL_MASK;
	return (WRGB_t) { .raw = rb | wg };
}

void init_gamma_table(float gamma) {
	float inv_gamma = 1.0f / gamma;
	for (int i = 1; i < 256; i++) {
		gammaT[i] = (int32_t)(powf((float)i / 255.0f, gamma) * 255.0f + 0.5f);
		inv_gammaT[i] = (int32_t)(powf(((float)i - 0.5f) / 255.0f, inv_gamma) * 255.0f + 0.5f);
	}
}

uint8_t gamma_correct(uint8_t value) {
	return gammaT[value];
}
