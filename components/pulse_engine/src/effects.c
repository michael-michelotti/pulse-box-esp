#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include "effects.h"
#include "audio.h"
#include "led_math.h"

/********* IDEAS *********/
// polar - spectral bins but magnitude bars are radial/polar
// rain
// multi-color pulse - waves/pulses from various origins based on frequency (red for high, blue for low)
// advanced color effects - responds to harmony, frequency content deltas, creates advanced effects
// rotating circle, concentric circles
// multi-color twinkle - twinkle with palette or color set?
// color loop - breathe effect that cycles through color set or palette
// snake game simulation
// draw mode - (processed by PC app, sent as raw data)
// maybe add mic and do mic-mode for all audio modes?
// acid/tie dye effect - concentric circles/waves radiating out, neon colors 
	// (called distortion waves in wled? similar one called flow = retro colored bars permeating from inside out. waving cell is similar)
// drift - rotating kaleidoscope
// color bursts / black hole? complex effects might use their particle system
// heartbeat
// matrix - bars, white fading to green to black
// polar lights - complex addition of sine wave (looks like an actual noise envelope - could make audio reactive?)
// https://github.com/wled/WLED - open source firmware with a ton of effect ideas and implementations

/************************/

/* Reference size for scaling: single panel edge length in pixels */
#define PANEL_SIZE 8


/***** BASS MAGNITUDE HELPER - EXTRACTS BASS ENERGY FROM FFT BINS 1-4 ********/
static float compute_bass_magnitude(const AudioState_t *audio, uint8_t sensitivity)
{
	float bass_mag = 0.0f;
	for (int i = 1; i <= 4; i++) {
		float real = audio->fft_bins[i * 2];
		float imag = audio->fft_bins[i * 2 + 1];
		bass_mag += sqrtf(real * real + imag * imag);
	}
	float db = 20.0f * log10f(bass_mag + 1.0f);
	/* sensitivity 0-100 controls dB floor:
	   100 = floor at 65 (very sensitive), 0 = floor at 95 (only hard bass hits).
	   30 dB window above floor maps to 0-255. */
	float db_floor = 105.0f - sensitivity * 0.3f;
	return fminf(fmaxf((db - db_floor) * (255.0f / 30.0f), 0.0f), 255.0f);
}


/***** BASS EFFECT - PULSES BRIGHTNESS BASED ON LOW FREQUENCY CONTENT ********/
#define BASS_MIN_BRIGHTNESS 		10.0f

static void bass_pulse_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

	float decay = 2.0f + params->speed * 16.0f;

	static float current_brightness = 0;
	static AudioState_t audio;
	audio_update(&audio);

	float bass = compute_bass_magnitude(&audio, params->sensitivity);

	float target = fminf(bass, 255.0f);

	if (target >= current_brightness) {
		current_brightness = target;
	} else {
		current_brightness -= decay;
		if (current_brightness < BASS_MIN_BRIGHTNESS)
			current_brightness = BASS_MIN_BRIGHTNESS;
	}

	WRGB_t color = dim_color(params->color_set->colors[0], (uint8_t)current_brightness);
	for (int i = 0; i < canvas->num_pixels; i++) {
		int idx = canvas->pixels[i].led_index * 3;
		framebuffer[idx + 0] = color.r;
		framebuffer[idx + 1] = color.g;
		framebuffer[idx + 2] = color.b;
	}
}


/***** BASS SPLASH EFFECT - WAVES PULSE OUTWARDS FROM CENTER ON HARD BASS ********/
#define MAX_WAVES 4
#define WAVE_WIDTH 2.0f

typedef struct {
    float radius;
    float intensity;
    uint8_t active;
} Wave_t;

static void bass_splash_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

	float wave_speed = 1.0f + params->speed * 14.0f;
	float wave_decay = 0.3f + params->speed * 2.7f;
	float threshold = 128.0f;

	static Wave_t waves[MAX_WAVES] = {0};
    static AudioState_t audio;
    static uint8_t was_below = 1;  // prevent rapid re-triggering

    audio_update(&audio);
    float bass = compute_bass_magnitude(&audio, params->sensitivity);

    // Spawn wave on bass hit (only if we dropped below threshold since last hit)
    if (bass > threshold && was_below) {
        for (int i = 0; i < MAX_WAVES; i++) {
            if (!waves[i].active) {
                waves[i].radius = 0;
                waves[i].intensity = 1.0f;
                waves[i].active = 1;
                was_below = 0;
                break;
            }
        }
    }
    if (bass < threshold * 0.7f) {
        was_below = 1;
    }

    // Update waves
    for (int i = 0; i < MAX_WAVES; i++) {
        if (!waves[i].active) continue;
        waves[i].radius += wave_speed * frame->dt;
        waves[i].intensity -= wave_decay * frame->dt;
        if (waves[i].intensity <= 0) waves[i].active = 0;
    }

    // Render — use global canvas center, scale wave width with canvas size
    WRGB_t base_color = params->color_set->colors[0];
    float center_x = (canvas->max_x + canvas->min_x) / 2.0f;
    float center_y = (canvas->max_y + canvas->min_y) / 2.0f;
    float cw = canvas->max_x - canvas->min_x + 1;
    float ch = canvas->max_y - canvas->min_y + 1;
    float diag = sqrtf(cw * cw + ch * ch);
    float wave_width = WAVE_WIDTH * (diag / (float)PANEL_SIZE);
    for (int i = 0; i < canvas->num_pixels; i++) {
    	float dx = canvas->pixels[i].x - center_x;
        float dy = canvas->pixels[i].y - center_y;
        float dist = sqrtf(dx * dx + dy * dy);
        float brightness = 0;

        for (int w = 0; w < MAX_WAVES; w++) {
            if (!waves[w].active) continue;
            float diff = fabsf(dist - waves[w].radius);
            if (diff < wave_width) {
                float contrib = waves[w].intensity * (1.0f - diff / wave_width);
                if (contrib > brightness) brightness = contrib;
            }
        }

        uint8_t bval = (uint8_t)(fminf(brightness, 1.0f) * 255.0f);
        int led = canvas->pixels[i].led_index * 3;
        WRGB_t dimmed = dim_color(base_color, bval);
        framebuffer[led + 0] = dimmed.r;
        framebuffer[led + 1] = dimmed.g;
        framebuffer[led + 2] = dimmed.b;
    }
}

/***** RAINBOW EFFECT - GENERATES RAINBOW (ROYGBIV) EVERY 7 LEDS WITH CONFIGURABLE SPEED ********/
static void rainbow_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	float dir_x = cosf(params->direction * M_PI / 180.0f);
	float dir_y = sinf(params->direction * M_PI / 180.0f);

	// Project all pixels onto direction vector to find min/max
	float min_proj = 1e9f, max_proj = -1e9f;
	for (int i = 0; i < canvas->num_pixels; i++) {
		float proj = canvas->pixels[i].x * dir_x + canvas->pixels[i].y * dir_y;
		if (proj < min_proj) min_proj = proj;
		if (proj > max_proj) max_proj = proj;
	}
	float range = max_proj - min_proj;
	if (range < 0.001f) range = 1.0f;  // avoid divide by zero

	for (int i = 0; i < canvas->num_pixels; i++) {
		float proj = canvas->pixels[i].x * dir_x + canvas->pixels[i].y * dir_y;
		float norm = (proj - min_proj) / range;
		float pos = fmodf(norm - frame->time * params->speed + 1.0f, 1.0f);
		uint8_t index = (uint8_t)(pos * 255.0f);

		int idx = canvas->pixels[i].led_index * 3;
		WRGB_t rgb = palette_color_at(params->palette, index);
		framebuffer[idx + 0] = rgb.r;
		framebuffer[idx + 1] = rgb.g;
		framebuffer[idx + 2] = rgb.b;
	}
}

/***** TWINKLE EFFECT - LIGHTS UP SPARKS_PER_FRAME LEDS AT RANDOM ********/
#define TWINKLE_SPARKS_PER_FRAME	1

static void twinkle_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	float decay = 0.4f + params->speed * 3.2f;

	static float brightness[MAX_PIXELS] = {0};

	// Randomly spark pixels
	for (int i = 0; i < TWINKLE_SPARKS_PER_FRAME; i++) {
		int idx = rand() % canvas->num_pixels;
		brightness[idx] = 1.0f;
	}

	// Decay and render
	WRGB_t base_color = params->color_set->colors[0];
	for (int i = 0; i < canvas->num_pixels; i++) {
		brightness[i] -= decay * frame->dt;
		if (brightness[i] < 0) brightness[i] = 0;

		uint8_t bval = (uint8_t)(brightness[i] * 255.0f);
		int led = canvas->pixels[i].led_index * 3;
		WRGB_t dimmed = dim_color(base_color, bval);
		framebuffer[led + 0] = dimmed.r;
		framebuffer[led + 1] = dimmed.g;
		framebuffer[led + 2] = dimmed.b;
	}
}


/***** SPARKLE EFFECT - PALETTE-COLORED RANDOM SPARKS ********/
#define SPARKLE_SPARKS_PER_FRAME	1

static void sparkle_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	float decay = 0.4f + params->speed * 3.2f;

	static float brightness[MAX_PIXELS] = {0};
	static uint8_t color_idx[MAX_PIXELS] = {0};

	// Randomly spark pixels with a random palette color
	for (int i = 0; i < SPARKLE_SPARKS_PER_FRAME; i++) {
		int idx = rand() % canvas->num_pixels;
		brightness[idx] = 1.0f;
		color_idx[idx] = (uint8_t)(rand() & 0xFF);
	}

	// Decay and render
	for (int i = 0; i < canvas->num_pixels; i++) {
		brightness[i] -= decay * frame->dt;
		if (brightness[i] < 0) brightness[i] = 0;

		uint8_t bval = (uint8_t)(brightness[i] * 255.0f);
		int led = canvas->pixels[i].led_index * 3;
		WRGB_t color = palette_color_at(params->palette, color_idx[i]);
		WRGB_t dimmed = dim_color(color, bval);
		framebuffer[led + 0] = dimmed.r;
		framebuffer[led + 1] = dimmed.g;
		framebuffer[led + 2] = dimmed.b;
	}
}

/***** SOLID EFFECT - LIGHTS UP SPARKS_PER_FRAME LEDS AT RANDOM ********/
static void solid_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	WRGB_t color = params->color_set->colors[0];
	for (int i = 0; i < canvas->num_pixels; i++) {
		int led = canvas->pixels[i].led_index * 3;
		framebuffer[led + 0] = color.r;
		framebuffer[led + 1] = color.g;
		framebuffer[led + 2] = color.b;
	}
}

/***** FIRE EFFECT - SPARKS GROW FROM BOTTOM OF FRAME, HEAT DISSIPATES AND COOLS ********/
#define FIRE_COOLING  70
#define FIRE_SPARKING 130

static void fire_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	static uint8_t heat[MAX_PIXELS] = {0};
	static uint8_t frame_skip = 0;
	frame_skip = !frame_skip;


	int w = canvas->max_x - canvas->min_x + 1;
	int h = canvas->max_y - canvas->min_y + 1;

	if (frame_skip) {
		// Step 1: Cool down every cell a little
		float center_x = (w - 1) / 2.0f;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float edge_factor = fabsf(x - center_x) / center_x; // 0 at center, 1 at edges
				int max_cool = (int)(((FIRE_COOLING * 10) / h + 2) * (0.55f + 0.45f * edge_factor));
				int cool = rand() % (max_cool + 1);
				int idx = y * w + x;
				heat[idx] = (heat[idx] > cool) ? heat[idx] - cool : 0;
			}
		}

		// Step 2: Heat rises — each cell fed by the two cells below it (lower y)
		for (int x = 0; x < w; x++) {
			for (int y = h - 1; y >= 2; y--) {
				heat[y * w + x] = (heat[(y-1) * w + x] +
						heat[(y-2) * w + x] +
						heat[(y-2) * w + x]) / 3;
			}
		}

		// Step 3: Randomly ignite sparks at the bottom (y=0,1,2)
		for (int x = 0; x < w; x++) {
			if ((rand() % 255) < FIRE_SPARKING) {
				int spark_y = rand() % 2;
				int new_heat = heat[spark_y * w + x] + (rand() % 96 + 160);
				heat[spark_y * w + x] = (new_heat > 255) ? 255 : (uint8_t)new_heat;
			}
		}
	}

	// Step 4: Map heat to palette color
	for (int i = 0; i < canvas->num_pixels; i++) {
		int x = canvas->pixels[i].x;
		int y = canvas->pixels[i].y;
		uint8_t colorindex = (heat[y * w + x] * 240) >> 8;
		int led = canvas->pixels[i].led_index * 3;
		WRGB_t rgb = palette_color_at(params->palette, colorindex);
		framebuffer[led + 0] = rgb.r;
		framebuffer[led + 1] = rgb.g;
		framebuffer[led + 2] = rgb.b;
	}
}

/***** BREATHE EFFECT - PRIMARY COLOR OSCILLATES WITH A SINE WAVE ********/
static void breathe_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    float breath = (sinf(frame->time * params->speed * M_PI) + 1.0f) * 0.5f;

    uint8_t bval = (uint8_t)(breath * 255.0f);
    WRGB_t color = dim_color(params->color_set->colors[0], bval);

    for (int i = 0; i < canvas->num_pixels; i++) {
        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = color.r;
        framebuffer[led + 1] = color.g;
        framebuffer[led + 2] = color.b;
    }
}

/***** GLOW EFFECT - BREATHE THROUGH PALETTE COLORS ********/
static void glow_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    // Continuously advance palette position over time
    float phase = frame->time * params->speed * 0.5f;
    // Use sine to create smooth brightness oscillation
    float breath = (sinf(phase * M_PI) + 1.0f) * 0.5f;
    // Advance palette index — each full breath cycle (2*PI) moves ~16 palette steps
    uint8_t index = (uint8_t)(phase * 8.0f);

    WRGB_t color = palette_color_at(params->palette, index);
    WRGB_t dimmed = dim_color(color, (uint8_t)(breath * 255.0f));

    for (int i = 0; i < canvas->num_pixels; i++) {
        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = dimmed.r;
        framebuffer[led + 1] = dimmed.g;
        framebuffer[led + 2] = dimmed.b;
    }
}

/***** HEARTBEAT EFFECT - DOUBLE-PULSE BRIGHTNESS PATTERN ********/
static void heartbeat_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    // Heartbeat cycle: two quick beats then a pause
    // Phase 0.0-0.15: first beat (up and down)
    // Phase 0.20-0.35: second beat (up and down)
    // Phase 0.35-1.0: rest
    float cycle = fmodf(frame->time * (0.15f + params->speed * 0.75f), 1.0f);
    float breath;

    if (cycle < 0.08f) {
        breath = cycle / 0.08f;               // first beat rise
    } else if (cycle < 0.16f) {
        breath = 1.0f - (cycle - 0.08f) / 0.08f * 0.5f; // first beat fall to 0.5
    } else if (cycle < 0.24f) {
        float t = (cycle - 0.16f) / 0.08f;
        breath = 0.5f + t * 0.3f;            // second beat rise (weaker, from 0.5 to 0.8)
    } else if (cycle < 0.36f) {
        float t = (cycle - 0.24f) / 0.12f;
        breath = 0.8f * (1.0f - t) + 0.3f * t; // second beat fall to resting
    } else {
        breath = 0.3f;                        // resting brightness
    }

    uint8_t bval = (uint8_t)(breath * 255.0f);
    WRGB_t color = dim_color(params->color_set->colors[0], bval);

    for (int i = 0; i < canvas->num_pixels; i++) {
        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = color.r;
        framebuffer[led + 1] = color.g;
        framebuffer[led + 2] = color.b;
    }
}

/***** WIPE EFFECT - GRID FILLS ONE LED AT A TIME, THEN EMPTIES ********/
static void wipe_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	static int lit_count = 0;
	static float accum = 0.0f;
	static float brightness = 255.0f;
	static bool filling = true;

	if (filling) {
		accum += frame->dt * params->speed * 20.0f;
		while (accum >= 1.0f) {
			lit_count++;
			accum -= 1.0f;
		}
		if (lit_count >= canvas->num_pixels) {
			lit_count = canvas->num_pixels;
			filling = false;
		}
	} else {
		brightness -= frame->dt * params->speed * 200.0f;
		if (brightness <= 0.0f) {
			brightness = 255.0f;
			filling = true;
			lit_count = 0;
			accum = 0.0f;
		}
	}

	WRGB_t color = dim_color(params->color_set->colors[0], (uint8_t)brightness);
	for (int i = 0; i < canvas->num_pixels; i++) {
		int led = canvas->pixels[i].led_index * 3;
		if (canvas->pixels[i].led_index < lit_count) {
			framebuffer[led + 0] = color.r;
			framebuffer[led + 1] = color.g;
			framebuffer[led + 2] = color.b;
		}
	}
}

/***** SPECTRUM EFFECT - SPECTRUM OF AUDIO INPUT FREQUENCY BINS ********/
static void spectrum_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	int w = canvas->max_x - canvas->min_x + 1;
	int h = canvas->max_y - canvas->min_y + 1;

	static AudioState_t audio;
	static float bar_heights[8] = {0};
	static int band_starts[9];
	static uint8_t initialized = 0;

	// Precompute log-spaced bin boundaries once (bins 1-512)
	if (!initialized) {
		for (int i = 0; i <= 8; i++) {
			band_starts[i] = (int)powf(2.0f, 9.0f * i / 8.0f);
			if (band_starts[i] < 1)   band_starts[i] = 1;
			if (band_starts[i] > 512) band_starts[i] = 512;
		}
		initialized = 1;
	}

	audio_update(&audio);

	float threshold = 150.0f - params->sensitivity * 1.0f;
	float decay = 2.0f + params->speed * 8.0f;

	for (int band = 0; band < 8; band++) {
		int bin_start = band_starts[band];
		int bin_end   = band_starts[band + 1];

		// Peak magnitude across bins in this band
		float magnitude = 0.0f;
		for (int bin = bin_start; bin < bin_end; bin++) {
			float real = audio.fft_bins[2 * bin];
			float imag = audio.fft_bins[2 * bin + 1];
			float mag = sqrtf(real * real + imag * imag);
			if (mag > magnitude) magnitude = mag;
		}

		// Scale to bar height 0-h
		float db = 20.0f * log10f(magnitude + 1.0f);
		float target = fminf(fmaxf((db - threshold) * ((float)h / 30.0f), 0.0f), (float)h);

		// Attack immediately, decay smoothly
		if (target > bar_heights[band]) {
			bar_heights[band] = target;
		} else {
			bar_heights[band] -= decay * frame->dt;
			if (bar_heights[band] < 0.0f) bar_heights[band] = 0.0f;
		}
	}

	// Render — map each pixel column to one of 8 bands
	for (int i = 0; i < canvas->num_pixels; i++) {
		int x = canvas->pixels[i].x - canvas->min_x;
		int y = canvas->pixels[i].y - canvas->min_y;
		int led = canvas->pixels[i].led_index * 3;

		int band = x * 8 / w;
		if (band > 7) band = 7;

		if ((float)y < bar_heights[band]) {
			uint8_t palette_index = (uint8_t)(band * 255 / 7);
			WRGB_t rgb = palette_color_at(params->palette, palette_index);
			framebuffer[led + 0] = rgb.r;
			framebuffer[led + 1] = rgb.g;
			framebuffer[led + 2] = rgb.b;
		}
	}
}


/***** IMAGE EFFECT - DISPLAYS PIXEL DATA STREAMED FROM DESKTOP APP ********/
extern uint8_t pixel_frame_buf[];
extern volatile bool pixel_frame_ready;

static void image_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	if (!pixel_frame_ready) return;

	int grid_w = canvas->max_x - canvas->min_x + 1;

	for (int i = 0; i < canvas->num_pixels; i++) {
		int x = canvas->pixels[i].x;
		int y = canvas->pixels[i].y;
		int src = (y * grid_w + x) * 3;
		int dst = canvas->pixels[i].led_index * 3;
		framebuffer[dst + 0] = pixel_frame_buf[src + 0];
		framebuffer[dst + 1] = pixel_frame_buf[src + 1];
		framebuffer[dst + 2] = pixel_frame_buf[src + 2];
	}
}

/***** TETRIS EFFECT - FALLING TETRIS PIECES THAT STACK UP ********/
#define TETRIS_MAX_W 16
#define TETRIS_MAX_H 16

typedef enum { TETRIS_O, TETRIS_L, TETRIS_I } TetrisPieceType;

typedef struct {
	int x, y;               // position (bottom-left of bounding box)
	TetrisPieceType type;
	uint8_t color_idx;
} TetrisPiece_t;

// Piece definitions: each piece is a list of (dx, dy) offsets from (x, y)
// O-piece (2x2 square):       (0,0) (1,0) (0,1) (1,1)
// L-piece (horizontal, tail up): (0,0) (1,0) (2,0) (2,1)
// I-piece (horizontal):        (0,0) (1,0) (2,0) (3,0)

static const int piece_offsets[3][4][2] = {
	[TETRIS_O] = { {0,0}, {1,0}, {0,1}, {1,1} },
	[TETRIS_L] = { {0,0}, {1,0}, {2,0}, {2,1} },
	[TETRIS_I] = { {0,0}, {1,0}, {2,0}, {3,0} },
};
static const int piece_sizes[3] = { 4, 4, 4 };
static const int piece_widths[3] = { 2, 3, 4 };
static const int piece_heights[3] = { 2, 2, 1 };

static bool tetris_check_collision(const uint8_t *board, int w, int h,
                                    TetrisPiece_t *piece, int dy) {
	for (int i = 0; i < piece_sizes[piece->type]; i++) {
		int px = piece->x + piece_offsets[piece->type][i][0];
		int py = piece->y + piece_offsets[piece->type][i][1] + dy;
		if (py < 0) return true;
		if (px < 0 || px >= w || py >= h) return true;
		if (board[py * w + px] != 0) return true;
	}
	return false;
}

static void tetris_place_piece(uint8_t *board, int w, TetrisPiece_t *piece) {
	for (int i = 0; i < piece_sizes[piece->type]; i++) {
		int px = piece->x + piece_offsets[piece->type][i][0];
		int py = piece->y + piece_offsets[piece->type][i][1];
		board[py * w + px] = piece->color_idx + 1; // 1-indexed color
	}
}

static void tetris_spawn_piece(TetrisPiece_t *piece, int w, int h, int num_colors) {
	piece->type = (TetrisPieceType)(rand() % 3);
	piece->color_idx = rand() % (num_colors > 3 ? 3 : num_colors);
	piece->x = rand() % (w - piece_widths[piece->type] + 1);
	piece->y = h - piece_heights[piece->type];
}

static void tetris_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

	int w = canvas->max_x - canvas->min_x + 1;
	int h = canvas->max_y - canvas->min_y + 1;
	if (w > TETRIS_MAX_W) w = TETRIS_MAX_W;
	if (h > TETRIS_MAX_H) h = TETRIS_MAX_H;

	// Board stores color index+1 for each cell (0 = empty)
	static uint8_t board[TETRIS_MAX_W * TETRIS_MAX_H];
	static TetrisPiece_t current;
	static float drop_accum = 0.0f;
	static bool need_spawn = true;
	static bool initialized = false;

	int num_colors = params->color_set->num_colors;
	if (num_colors < 1) num_colors = 1;

	if (!initialized) {
		for (int i = 0; i < TETRIS_MAX_W * TETRIS_MAX_H; i++) board[i] = 0;
		need_spawn = true;
		initialized = true;
	}

	// Spawn new piece if needed
	if (need_spawn) {
		tetris_spawn_piece(&current, w, h, num_colors);
		// If spawn position collides, board is full — reset
		if (tetris_check_collision(board, w, h, &current, 0)) {
			for (int i = 0; i < TETRIS_MAX_W * TETRIS_MAX_H; i++) board[i] = 0;
			tetris_spawn_piece(&current, w, h, num_colors);
		}
		need_spawn = false;
		drop_accum = 0.0f;
	}

	// Drop speed: speed 0 = 1 row/sec, speed 1 = 10 rows/sec
	float drop_rate = 1.0f + params->speed * 9.0f;
	drop_accum += frame->dt * drop_rate;

	while (drop_accum >= 1.0f) {
		drop_accum -= 1.0f;
		if (!tetris_check_collision(board, w, h, &current, -1)) {
			current.y--;
		} else {
			// Land the piece
			tetris_place_piece(board, w, &current);
			need_spawn = true;
			break;
		}
	}

	// Render board
	for (int i = 0; i < canvas->num_pixels; i++) {
		int x = canvas->pixels[i].x - canvas->min_x;
		int y = canvas->pixels[i].y - canvas->min_y;
		int led = canvas->pixels[i].led_index * 3;

		uint8_t cell = 0;
		if (x >= 0 && x < w && y >= 0 && y < h)
			cell = board[y * w + x];

		if (cell > 0) {
			WRGB_t c = params->color_set->colors[(cell - 1) % num_colors];
			framebuffer[led + 0] = c.r;
			framebuffer[led + 1] = c.g;
			framebuffer[led + 2] = c.b;
		} else {
			framebuffer[led + 0] = 0;
			framebuffer[led + 1] = 0;
			framebuffer[led + 2] = 0;
		}
	}

	// Render falling piece on top
	if (!need_spawn) {
		WRGB_t pc = params->color_set->colors[current.color_idx % num_colors];
		for (int i = 0; i < piece_sizes[current.type]; i++) {
			int px = current.x + piece_offsets[current.type][i][0] + canvas->min_x;
			int py = current.y + piece_offsets[current.type][i][1] + canvas->min_y;
			// Find the pixel at (px, py) and draw it
			for (int j = 0; j < canvas->num_pixels; j++) {
				if (canvas->pixels[j].x == px && canvas->pixels[j].y == py) {
					int led = canvas->pixels[j].led_index * 3;
					framebuffer[led + 0] = pc.r;
					framebuffer[led + 1] = pc.g;
					framebuffer[led + 2] = pc.b;
					break;
				}
			}
		}
	}
}

/***** TUNNEL EFFECT - CONCENTRIC SQUARES RADIATING OUTWARD USING COLOR SET ********/
static void tunnel_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    float center_x = (canvas->max_x + canvas->min_x) / 2.0f;
    float center_y = (canvas->max_y + canvas->min_y) / 2.0f;
    int num_colors = params->color_set->num_colors;
    if (num_colors < 1) num_colors = 1;
    float tw = canvas->max_x - canvas->min_x + 1;
    float th = canvas->max_y - canvas->min_y + 1;
    float ring_scale = fmaxf(tw, th) / (float)PANEL_SIZE;

    static float phase = 0.0f;
    static float accum = 0.0f;
    accum += frame->dt * (0.5f + params->speed * 4.0f);
    while (accum >= 1.0f) {
        phase += 1.0f;
        accum -= 1.0f;
    }

    int phase_int = (int)phase;

    for (int i = 0; i < canvas->num_pixels; i++) {
        float dx = fabsf(canvas->pixels[i].x - center_x);
        float dy = fabsf(canvas->pixels[i].y - center_y);
        int ring = (int)(fmaxf(dx, dy) / ring_scale);

        int color_idx = ((ring - phase_int) % num_colors + num_colors) % num_colors;
        WRGB_t c = params->color_set->colors[color_idx];

        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = c.r;
        framebuffer[led + 1] = c.g;
        framebuffer[led + 2] = c.b;
    }
}

/***** MATRIX EFFECT - RANDOMLY FALLING BLOCKS WITH BRIGHT HEAD AND FADING TRAIL ********/
#define MATRIX_MAX_W 32
#define MATRIX_MAX_H 32

static void matrix_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    int w = canvas->max_x - canvas->min_x + 1;
    int h = canvas->max_y - canvas->min_y + 1;
    if (w > MATRIX_MAX_W) w = MATRIX_MAX_W;
    if (h > MATRIX_MAX_H) h = MATRIX_MAX_H;

    static float drop_y[MATRIX_MAX_W];
    static float spawn_timer[MATRIX_MAX_W];
    static uint8_t trail[MATRIX_MAX_W * MATRIX_MAX_H];
    static bool initialized = false;

    if (!initialized) {
        for (int x = 0; x < MATRIX_MAX_W; x++) {
            drop_y[x] = (float)h;
            spawn_timer[x] = (float)(rand() % 100) / 100.0f * 2.0f;
        }
        for (int i = 0; i < MATRIX_MAX_W * MATRIX_MAX_H; i++) trail[i] = 0;
        initialized = true;
    }

    float fall_speed = 2.0f + params->speed * 12.0f;
    float trail_decay = 300.0f + params->speed * 600.0f;

    // Decay all trails
    for (int i = 0; i < w * h; i++) {
        int dec = (int)(trail_decay * frame->dt);
        trail[i] = (trail[i] > dec) ? trail[i] - dec : 0;
    }

    // Update each column
    for (int x = 0; x < w; x++) {
        if (drop_y[x] >= h) {
            spawn_timer[x] -= frame->dt;
            if (spawn_timer[x] <= 0) {
                drop_y[x] = (float)(h - 1);
                spawn_timer[x] = 0.5f + (float)(rand() % 200) / 100.0f;
            }
        } else {
            drop_y[x] -= fall_speed * frame->dt;
            int iy = (int)(drop_y[x] + 0.5f);
            if (iy >= 0 && iy < h) {
                trail[iy * w + x] = 255;
            }
            if (drop_y[x] < -1.0f) {
                drop_y[x] = (float)h;
            }
        }
    }

    // Render: white head → user color → black
    WRGB_t white = { .r = 255, .g = 255, .b = 255 };
    WRGB_t base = params->color_set->colors[0];
    for (int i = 0; i < canvas->num_pixels; i++) {
        int x = canvas->pixels[i].x - canvas->min_x;
        int y = canvas->pixels[i].y - canvas->min_y;
        int led = canvas->pixels[i].led_index * 3;

        if (x >= 0 && x < w && y >= 0 && y < h) {
            uint8_t val = trail[y * w + x];
            if (val > 200) {
                // Blend from base color toward white for the head
                uint8_t head_blend = (uint8_t)((val - 200) * (255 / 55));
                WRGB_t c = blend_colors(base, white, head_blend);
                framebuffer[led + 0] = c.r;
                framebuffer[led + 1] = c.g;
                framebuffer[led + 2] = c.b;
            } else {
                WRGB_t c = dim_color(base, val);
                framebuffer[led + 0] = c.r;
                framebuffer[led + 1] = c.g;
                framebuffer[led + 2] = c.b;
            }
        }
    }
}

/***** PLASMA EFFECT - SINE WAVE INTERFERENCE PATTERN WITH PALETTE COLORING ********/
static void plasma_compute(const Canvas_t *canvas, const FrameState_t *frame,
        const EffectParams_t *params, const Mapping_t *mapping,
        uint8_t *framebuffer) {

    float t = frame->time * (0.3f + params->speed * 2.0f);

    float pw = (float)(canvas->max_x - canvas->min_x + 1);
    float ph = (float)(canvas->max_y - canvas->min_y + 1);
    /* Scale factors to normalize coordinates to reference PANEL_SIZE */
    float sx = (float)PANEL_SIZE / pw;
    float sy = (float)PANEL_SIZE / ph;
    float ncx = (float)PANEL_SIZE * 0.5f;
    float ncy = (float)PANEL_SIZE * 0.5f;

    for (int i = 0; i < canvas->num_pixels; i++) {
        float x = (float)canvas->pixels[i].x * sx;
        float y = (float)canvas->pixels[i].y * sy;

        // Layer 1: horizontal sine
        float v = sinf(x * 0.8f + t * 1.3f);
        // Layer 2: vertical sine
        v += sinf(y * 0.9f + t * 0.9f);
        // Layer 3: diagonal sine
        v += sinf((x + y) * 0.6f + t * 0.7f);
        // Layer 4: radial sine from center
        float dx = x - ncx + sinf(t * 0.4f) * 2.0f;
        float dy = y - ncy + cosf(t * 0.3f) * 2.0f;
        v += sinf(sqrtf(dx * dx + dy * dy) * 1.0f - t * 1.1f);

        // v is in range [-4, 4], map to [0, 255]
        uint8_t index = (uint8_t)((v + 4.0f) * 31.875f); // 255/8 = 31.875

        int led = canvas->pixels[i].led_index * 3;
        WRGB_t rgb = palette_color_at(params->palette, index);
        framebuffer[led + 0] = rgb.r;
        framebuffer[led + 1] = rgb.g;
        framebuffer[led + 2] = rgb.b;
    }
}

/* Export of all available effects; must also be externed in effects.h, effects.h include gives access to all effects */
const Effect_t bass_pulse_effect = { .name = "bass", .compute = bass_pulse_compute };
const Effect_t rainbow_effect = { .name = "rainbow", .compute = rainbow_compute };
const Effect_t twinkle_effect = { .name = "twinkle", .compute = twinkle_compute };
const Effect_t solid_effect = { .name = "solid", .compute = solid_compute };
const Effect_t bass_splash_effect = { .name = "splash", .compute = bass_splash_compute };
const Effect_t fire_effect = { .name = "fire", .compute = fire_compute };
const Effect_t breathe_effect = { .name = "breathe", .compute = breathe_compute };
const Effect_t wipe_effect = { .name = "wipe", .compute = wipe_compute };
const Effect_t spectrum_effect = { .name = "spectrum", .compute = spectrum_compute };
const Effect_t image_effect = { .name = "image", .skip_gamma = 1, .compute = image_compute };
const Effect_t gif_effect = { .name = "gif", .skip_gamma = 1, .compute = image_compute };
const Effect_t tunnel_effect = { .name = "tunnel", .compute = tunnel_compute };
const Effect_t tetris_effect = { .name = "tetris", .compute = tetris_compute };
const Effect_t plasma_effect = { .name = "plasma", .compute = plasma_compute };
const Effect_t sparkle_effect = { .name = "sparkle", .compute = sparkle_compute };
const Effect_t glow_effect = { .name = "glow", .compute = glow_compute };
const Effect_t matrix_effect = { .name = "matrix", .compute = matrix_compute };
const Effect_t heartbeat_effect = { .name = "heartbeat", .compute = heartbeat_compute };
