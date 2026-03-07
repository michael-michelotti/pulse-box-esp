#include <string.h>
#include "lwip/sockets.h"
#include "cmd_protocol.h"
#include "effects.h"
#include "led_math.h"
#include "canvas.h"

/* Externs from main.c */
extern EffectParams_t effect_params;
extern const Effect_t *current_effect;
extern Canvas_t canvas;
extern bool wifi_is_ap_mode;

int proto_recv_exact(int sock, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int n = recv(sock, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= n;
    }
    return 0;
}

int proto_send_frame(int sock, uint8_t type, const void *payload, uint16_t length)
{
    FrameHeader_t hdr = { .type = type, .length = length };
    if (send(sock, &hdr, FRAME_HEADER_SIZE, 0) != FRAME_HEADER_SIZE)
        return -1;
    if (length > 0) {
        if (send(sock, payload, length, 0) != (int)length)
            return -1;
    }
    return 0;
}

int proto_build_status(uint8_t *buf, size_t buf_size)
{
    const char *ename = current_effect->name;
    uint8_t ename_len = (uint8_t)strlen(ename);
    const char *pname = effect_params.palette->name;
    uint8_t pname_len = (uint8_t)strlen(pname);

    size_t total = STATUS_FIXED_SIZE + 1 + ename_len + 1 + pname_len;
    if (total > buf_size) return -1;

    StatusFixed_t fixed = {
        .protocol_ver = PROTOCOL_VERSION,
        .wifi_mode    = wifi_is_ap_mode ? 1 : 0,
        .brightness   = effect_params.brightness,
        .color_r      = effect_params.color_set->colors[0].r,
        .color_g      = effect_params.color_set->colors[0].g,
        .color_b      = effect_params.color_set->colors[0].b,
        .color2_r     = effect_params.color_set->colors[1].r,
        .color2_g     = effect_params.color_set->colors[1].g,
        .color2_b     = effect_params.color_set->colors[1].b,
        .color3_r     = effect_params.color_set->colors[2].r,
        .color3_g     = effect_params.color_set->colors[2].g,
        .color3_b     = effect_params.color_set->colors[2].b,
        .speed        = effect_params.speed,
        .direction    = effect_params.direction,
        .grid_width   = (uint8_t)(canvas.max_x - canvas.min_x + 1),
        .grid_height  = (uint8_t)(canvas.max_y - canvas.min_y + 1),
        .num_pixels   = canvas.num_pixels,
        .sensitivity  = effect_params.sensitivity,
    };

    int offset = 0;
    memcpy(buf + offset, &fixed, STATUS_FIXED_SIZE);
    offset += STATUS_FIXED_SIZE;
    buf[offset++] = ename_len;
    memcpy(buf + offset, ename, ename_len);
    offset += ename_len;
    buf[offset++] = pname_len;
    memcpy(buf + offset, pname, pname_len);
    offset += pname_len;

    return offset;
}
