#ifndef INC_LED_DRIVER_H_
#define INC_LED_DRIVER_H_

#include <stdint.h>

typedef struct {
	const char *name;
	void (*init)(void);
	void (*send_frame)(const uint8_t *framebuffer, uint16_t num_pixels);
} LedDriver_t;

// extern const LedDriver_t ws2812b_driver;

#endif /* INC_LED_DRIVER_H_ */
