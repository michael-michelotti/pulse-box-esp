#ifndef TCP_CMD_SERVER_H
#define TCP_CMD_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "canvas.h"

#define TCP_CMD_PORT 5001

void tcp_cmd_server_task(void *pvParameters);

/**
 * Push current device STATUS to the connected TCP client (if any).
 * Safe to call from any core-0 task (console, render).
 * No-op if no client is connected.
 */
void tcp_push_status(void);

/**
 * Push a live preview framebuffer to the connected TCP client (if any).
 * Reads the post-brightness, pre-gamma framebuffer from the renderer.
 * Safe to call from the render task on core 0.
 * No-op if no client is connected.
 */
void tcp_push_preview(void);

/* Pixel frame buffer — written by TCP server, read by image effect */
extern uint8_t pixel_frame_buf[MAX_PIXELS * 3];
extern volatile bool pixel_frame_ready;

#endif /* TCP_CMD_SERVER_H */
