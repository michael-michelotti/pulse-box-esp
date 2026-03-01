#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

#include "effects.h"
#include "audio.h"
#include "led_math.h"

/********* IDEAS *********/
// polar - spectral bins but magnitude bars are radial/polar
// rain
// multi-color pulse - waves/pulses from various origins based on frequency (red for high, blue for low)
// advanced color effects - responds to harmony, frequency content deltas, creates advanced effects
// rotating circle, concentric circles
// tunnel - effect of moving through a tunnel by generating various shapes and growing them to make them appear "closer"
// tetris - randomly falling tetris pieces
// multi-color twinkle
// color bars (rainbow but not all colors in the spectrum) - width should control band width
// image (processed by PC app)
// video (processed by PC app, sent as raw data)
// snake game simulation
// draw mode - (processed by PC app, sent as raw data)
// maybe add mic and do mic-mode for all audio modes?
// acid/tie dye effect - concentric circles/waves radiating out, neon colors
// https://github.com/wled/WLED - open source firmware with a ton of effect ideas and implementations

/************************/


/***** BASS MAGNITUDE HELPER - EXTRACTS BASS ENERGY FROM FFT BINS 1-4 ********/
static float compute_bass_magnitude(const AudioState_t *audio)
{
	float bass_mag = 0.0f;
	for (int i = 1; i <= 4; i++) {
		float real = audio->fft_bins[i * 2];
		float imag = audio->fft_bins[i * 2 + 1];
		bass_mag += sqrtf(real * real + imag * imag);
	}
	float db = 20.0f * log10f(bass_mag + 1.0f);
	return fminf(fmaxf((db - 60.0f) * (255.0f / 40.0f), 0.0f), 255.0f);
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

	float bass = compute_bass_magnitude(&audio);
	float scale = 0.5f + params->sensitivity * 0.015f;
	float target = fminf(bass * scale, 255.0f);
	// float target = fminf(audio.bass_magnitude, 255.0f);

	if (target >= current_brightness) {
		current_brightness = target;
	} else {
		current_brightness -= decay;
		if (current_brightness < BASS_MIN_BRIGHTNESS)
			current_brightness = BASS_MIN_BRIGHTNESS;
	}

	uint8_t brightness = (uint8_t)current_brightness;
	for (int i = 0; i < canvas->num_pixels; i++) {
		int idx = canvas->pixels[i].led_index * 3;
			framebuffer[idx + 0] = (params->color_set->colors[0].r * brightness) / 255;
			framebuffer[idx + 1] = (params->color_set->colors[0].g * brightness) / 255;
			framebuffer[idx + 2] = (params->color_set->colors[0].b * brightness) / 255;
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
	float threshold = 150.0f - params->sensitivity * 1.5f;

	static Wave_t waves[MAX_WAVES] = {0};
    static AudioState_t audio;
    static uint8_t was_below = 1;  // prevent rapid re-triggering

    audio_update(&audio);
    float bass = compute_bass_magnitude(&audio);

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

    // Render
    float center_x = canvas->panels[0].center_x;
    float center_y = canvas->panels[0].center_y;
    for (int i = 0; i < canvas->num_pixels; i++) {
    	float dx = canvas->pixels[i].x - center_x;
        float dy = canvas->pixels[i].y - center_y;
        float dist = sqrtf(dx * dx + dy * dy);
        float brightness = 0;

        for (int w = 0; w < MAX_WAVES; w++) {
            if (!waves[w].active) continue;
            float diff = fabsf(dist - waves[w].radius);
            if (diff < WAVE_WIDTH) {
                float contrib = waves[w].intensity * (1.0f - diff / WAVE_WIDTH);
                if (contrib > brightness) brightness = contrib;
            }
        }

        uint8_t b = (uint8_t)(fminf(brightness, 1.0f) * 255.0f);
        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = (params->color_set->colors[0].r * b) / 255;
        framebuffer[led + 1] = (params->color_set->colors[0].g * b) / 255;
        framebuffer[led + 2] = (params->color_set->colors[0].b * b) / 255;
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
	for (int i = 0; i < canvas->num_pixels; i++) {
		brightness[i] -= decay * frame->dt;
		if (brightness[i] < 0) brightness[i] = 0;

		uint8_t b = (uint8_t)(brightness[i] * 255.0f);
		int led = canvas->pixels[i].led_index * 3;
		framebuffer[led + 0] = (params->color_set->colors[0].r * b) / 255;
		framebuffer[led + 1] = (params->color_set->colors[0].g * b) / 255;
		framebuffer[led + 2] = (params->color_set->colors[0].b * b) / 255;
	}
}


/***** SOLID EFFECT - LIGHTS UP SPARKS_PER_FRAME LEDS AT RANDOM ********/
static void solid_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

	for (int i = 0; i < canvas->num_pixels; i++) {
		int led = canvas->pixels[i].led_index * 3;
		framebuffer[led + 0] = params->color_set->colors[0].r;
		framebuffer[led + 1] = params->color_set->colors[0].g;
		framebuffer[led + 2] = params->color_set->colors[0].b;
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
    uint8_t b = (uint8_t)(breath * 255.0f);

    WRGB_t color = params->color_set->colors[0];

    for (int i = 0; i < canvas->num_pixels; i++) {
        int led = canvas->pixels[i].led_index * 3;
        framebuffer[led + 0] = (color.r * b) / 255;
        framebuffer[led + 1] = (color.g * b) / 255;
        framebuffer[led + 2] = (color.b * b) / 255;
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

	WRGB_t color = params->color_set->colors[0];

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

	uint8_t b = (uint8_t)brightness;
	for (int i = 0; i < canvas->num_pixels; i++) {
		int led = canvas->pixels[i].led_index * 3;
		if (canvas->pixels[i].led_index < lit_count) {
			framebuffer[led + 0] = (color.r * b) / 255;
			framebuffer[led + 1] = (color.g * b) / 255;
			framebuffer[led + 2] = (color.b * b) / 255;
		}
	}
}

/***** SPECTRUM EFFECT - SPECTRUM OF AUDIO INPUT FREQUENCY BINS ********/
static void spectrum_compute(const Canvas_t *canvas, const FrameState_t *frame,
		const EffectParams_t *params, const Mapping_t *mapping,
		uint8_t *framebuffer) {

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

		// Scale to bar height 0-8
		float db = 20.0f * log10f(magnitude + 1.0f);
		float target = fminf(fmaxf((db - threshold) * (8.0f / 30.0f), 0.0f), 8.0f);

		// Attack immediately, decay smoothly
		if (target > bar_heights[band]) {
			bar_heights[band] = target;
		} else {
			bar_heights[band] -= decay * frame->dt;
			if (bar_heights[band] < 0.0f) bar_heights[band] = 0.0f;
		}
	}

	// Render — each column is one band, color by column from palette
	for (int i = 0; i < canvas->num_pixels; i++) {
		int x = canvas->pixels[i].x;
		int y = canvas->pixels[i].y;
		int led = canvas->pixels[i].led_index * 3;

		if ((float)y < bar_heights[x]) {
			uint8_t palette_index = (uint8_t)(x * 255 / 7);
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
const Effect_t image_effect = { .name = "image", .compute = image_compute };
const Effect_t gif_effect = { .name = "gif", .compute = image_compute };
