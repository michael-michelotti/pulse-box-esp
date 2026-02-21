#ifndef ESP_LED_DRIVER_H
#define ESP_LED_DRIVER_H

#include "led_driver.h"

#define LED_STRIP_GPIO      14
#define LED_STRIP_MAX_LEDS  512   // Future: 8 panels x 64 LEDs

extern const LedDriver_t esp32_ws2812b_driver;

#endif
