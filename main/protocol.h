#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* --- Message type IDs --- */

/* Client (desktop) -> Server (ESP32): 0x00-0x7F */
#define MSG_HELLO        0x01
#define MSG_CMD          0x02
#define MSG_PIXEL_FRAME  0x03

/* Server (ESP32) -> Client (desktop): 0x80-0xFF */
#define MSG_STATUS        0x81
#define MSG_CMD_RESP      0x82
#define MSG_PREVIEW_FRAME 0x83

#define PROTOCOL_VERSION 1

/* --- Frame format --- */

#define FRAME_HEADER_SIZE  3
#define FRAME_MAX_PAYLOAD  2048

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint16_t length;    /* little-endian, native on ESP32 + x86 */
} FrameHeader_t;

/* --- STATUS payload fixed portion (18 bytes) --- */

typedef struct __attribute__((packed)) {
    uint8_t  protocol_ver;
    uint8_t  wifi_mode;     /* 0 = STA, 1 = AP */
    uint8_t  brightness;    /* 0-100 */
    uint8_t  color_r;
    uint8_t  color_g;
    uint8_t  color_b;
    float    speed;         /* 0.0-1.0 */
    float    direction;     /* 0-360 */
    uint8_t  grid_width;
    uint8_t  grid_height;
    uint16_t num_pixels;
} StatusFixed_t;

#define STATUS_FIXED_SIZE sizeof(StatusFixed_t)

/* --- Protocol utility functions --- */

/**
 * Read exactly `len` bytes from socket, handling partial reads.
 * Returns 0 on success, -1 on error/disconnect.
 */
int proto_recv_exact(int sock, void *buf, size_t len);

/**
 * Send a complete TLV frame (header + payload).
 * Returns 0 on success, -1 on error.
 */
int proto_send_frame(int sock, uint8_t type, const void *payload, uint16_t length);

/**
 * Build a STATUS payload into the provided buffer.
 * Returns total payload size on success, -1 if buffer too small.
 */
int proto_build_status(uint8_t *buf, size_t buf_size);

#endif /* PROTOCOL_H */
