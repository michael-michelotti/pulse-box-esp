#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "tcp_cmd_server.h"
#include "console.h"
#include "protocol.h"

extern Canvas_t canvas;

static const char *TAG = "tcp_cmd";

/* Pixel frame buffer for image effect */
uint8_t pixel_frame_buf[MAX_PIXELS * 3];
volatile bool pixel_frame_ready = false;

/* Client socket for STATUS push from other tasks (-1 = no client) */
static volatile int tcp_client_sock = -1;

/* Static buffers to avoid large stack allocations (single client model) */
static uint8_t payload_buf[FRAME_MAX_PAYLOAD];
static char cmd_text[256];
static char resp_text[1024];
static uint8_t resp_buf[1 + 1024]; /* success byte + response text */
static uint8_t status_buf[128];

static void handle_client(int client_sock)
{
    FrameHeader_t hdr;

    ESP_LOGI(TAG, "Client connected (TLV protocol)");
    tcp_client_sock = client_sock;

    while (1) {
        /* Read 3-byte frame header */
        if (proto_recv_exact(client_sock, &hdr, FRAME_HEADER_SIZE) != 0) {
            ESP_LOGI(TAG, "Client disconnected");
            break;
        }

        /* Validate payload length */
        if (hdr.length > FRAME_MAX_PAYLOAD) {
            ESP_LOGE(TAG, "Payload too large: %u", hdr.length);
            break;
        }

        /* Read payload */
        if (hdr.length > 0) {
            if (proto_recv_exact(client_sock, payload_buf, hdr.length) != 0) {
                ESP_LOGE(TAG, "Payload recv failed");
                break;
            }
        }

        switch (hdr.type) {
        case MSG_HELLO: {
            int slen = proto_build_status(status_buf, sizeof(status_buf));
            if (slen > 0) {
                proto_send_frame(client_sock, MSG_STATUS, status_buf, slen);
            }
            break;
        }

        case MSG_CMD: {
            /* Extract text command from payload */
            uint16_t cmd_len = hdr.length < sizeof(cmd_text) - 1
                             ? hdr.length : sizeof(cmd_text) - 1;
            memcpy(cmd_text, payload_buf, cmd_len);
            cmd_text[cmd_len] = '\0';

            /* Process command using existing shared handler */
            bool ok = process_user_command(cmd_text, resp_text, sizeof(resp_text));

            /* Send CMD_RESP: [success: u8][response_text: ...] */
            uint16_t text_len = (uint16_t)strlen(resp_text);
            resp_buf[0] = ok ? 1 : 0;
            memcpy(resp_buf + 1, resp_text, text_len);
            proto_send_frame(client_sock, MSG_CMD_RESP, resp_buf, 1 + text_len);

            /* Push STATUS after state-changing commands */
            if (strncmp(cmd_text, "help", 4) != 0 &&
                strncmp(cmd_text, "status", 6) != 0) {
                int slen = proto_build_status(status_buf, sizeof(status_buf));
                if (slen > 0) {
                    proto_send_frame(client_sock, MSG_STATUS, status_buf, slen);
                }
            }
            break;
        }

        case MSG_PIXEL_FRAME: {
            uint16_t expected = canvas.num_pixels * 3;
            if (hdr.length == expected) {
                memcpy(pixel_frame_buf, payload_buf, expected);
                pixel_frame_ready = true;
            } else {
                ESP_LOGW(TAG, "PIXEL_FRAME size mismatch: got %u, expected %u",
                         hdr.length, expected);
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown message type: 0x%02x", hdr.type);
            break;
        }
    }

    tcp_client_sock = -1;
    pixel_frame_ready = false;
    close(client_sock);
}

void tcp_push_status(void)
{
    int sock = tcp_client_sock;
    if (sock < 0) return;

    uint8_t buf[128];
    int slen = proto_build_status(buf, sizeof(buf));
    if (slen > 0) {
        proto_send_frame(sock, MSG_STATUS, buf, slen);
    }
}

void tcp_cmd_server_task(void *pvParameters)
{
    ESP_LOGI(TAG, "TCP command server starting on port %d, core %d",
             TCP_CMD_PORT, xPortGetCoreID());

    while (1) {
        /* Create listening socket */
        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "socket() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(TCP_CMD_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "bind() failed: errno %d", errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(listen_sock, 1) < 0) {
            ESP_LOGE(TAG, "listen() failed: errno %d", errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Listening on port %d", TCP_CMD_PORT);

        while (1) {
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);

            if (client_sock < 0) {
                ESP_LOGE(TAG, "accept() failed: errno %d", errno);
                break;
            }

            ESP_LOGI(TAG, "Client connected from " IPSTR, IP2STR((esp_ip4_addr_t *)&client_addr.sin_addr));
            handle_client(client_sock);
            /* After client disconnects, loop back to accept the next one */
        }

        close(listen_sock);
    }
}
