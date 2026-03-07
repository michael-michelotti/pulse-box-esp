#include <string.h>
#include <stdio.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "panel_bus.h"
#include "canvas.h"

static const char *TAG = "panel_bus";

extern Canvas_t canvas;

/* Controller's two panel links */
static PbLink_t link_north = {
    .uart_num   = PB_NORTH_UART_NUM,
    .side       = PB_SIDE_NORTH,
    .sense_gpio = PB_NORTH_SENSE_GPIO,
};

static PbLink_t link_east = {
    .uart_num   = PB_EAST_UART_NUM,
    .side       = PB_SIDE_EAST,
    .sense_gpio = PB_EAST_SENSE_GPIO,
};

/* Global topology state */
static PbTopology_t topology;

/* Flag to prevent render task from reading canvas mid-update */
volatile bool canvas_updating = false;

/* ------------------------------------------------------------------ */
/*  CRC-8 (polynomial 0x07, init 0x00)                               */
/* ------------------------------------------------------------------ */

uint8_t pb_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/*  UART initialization                                               */
/* ------------------------------------------------------------------ */

static void init_uart_link(const PbLink_t *link, int tx_gpio, int rx_gpio)
{
    uart_config_t uart_config = {
        .baud_rate = PB_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_driver_install(link->uart_num, PB_UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(link->uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(link->uart_num, tx_gpio, rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d initialized (TX=%d, RX=%d) for %s link",
             link->uart_num, tx_gpio, rx_gpio,
             link->side == PB_SIDE_NORTH ? "NORTH" : "EAST");
}

static void init_sense_gpio(int gpio)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

void panel_bus_init(void)
{
    init_uart_link(&link_north, PB_NORTH_TX_GPIO, PB_NORTH_RX_GPIO);
    init_uart_link(&link_east, PB_EAST_TX_GPIO, PB_EAST_RX_GPIO);

    init_sense_gpio(PB_NORTH_SENSE_GPIO);
    init_sense_gpio(PB_EAST_SENSE_GPIO);

    memset(&topology, 0, sizeof(topology));
    topology.next_addr = 1;

    ESP_LOGI(TAG, "Panel bus initialized");
}

/* ------------------------------------------------------------------ */
/*  Frame I/O                                                         */
/* ------------------------------------------------------------------ */

int pb_send_frame(const PbLink_t *link, uint8_t dst, uint8_t src,
                  uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[PB_MAX_FRAME_SIZE];

    if (len > PB_MAX_PAYLOAD) return -1;

    frame[0] = PB_SYNC_BYTE;
    frame[1] = dst;
    frame[2] = src;
    frame[3] = type;
    frame[4] = len;

    if (len > 0 && payload != NULL) {
        memcpy(&frame[5], payload, len);
    }

    /* CRC over DST through end of payload (skip SYNC) */
    frame[5 + len] = pb_crc8(&frame[1], 4 + len);

    int total = PB_HEADER_SIZE + len + 1;
    int written = uart_write_bytes(link->uart_num, frame, total);
    if (written != total) {
        ESP_LOGE(TAG, "UART%d write failed: wrote %d/%d", link->uart_num, written, total);
        return -1;
    }

    return 0;
}

int pb_recv_frame(const PbLink_t *link, PbFrameHeader_t *hdr,
                  uint8_t *payload, uint8_t max_len, int timeout_ms)
{
    uint8_t byte;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    /* Scan for sync byte */
    while (1) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) return -1;

        int ms = (int)(remaining_us / 1000);
        if (ms < 1) ms = 1;

        int n = uart_read_bytes(link->uart_num, &byte, 1, pdMS_TO_TICKS(ms));
        if (n <= 0) return -1;
        if (byte == PB_SYNC_BYTE) break;
    }

    /* Read remaining header (DST, SRC, TYPE, LEN) */
    uint8_t hdr_rest[4];
    {
        int64_t remaining_us = deadline - esp_timer_get_time();
        int ms = (remaining_us > 0) ? (int)(remaining_us / 1000) : 1;
        if (ms < 1) ms = 1;

        int n = uart_read_bytes(link->uart_num, hdr_rest, 4, pdMS_TO_TICKS(ms));
        if (n != 4) return -1;
    }

    hdr->sync = PB_SYNC_BYTE;
    hdr->dst  = hdr_rest[0];
    hdr->src  = hdr_rest[1];
    hdr->type = hdr_rest[2];
    hdr->len  = hdr_rest[3];

    if (hdr->len > max_len || hdr->len > PB_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Frame too large: len=%u", hdr->len);
        return -1;
    }

    /* Read payload */
    if (hdr->len > 0) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        int ms = (remaining_us > 0) ? (int)(remaining_us / 1000) : 1;
        if (ms < 1) ms = 1;

        int n = uart_read_bytes(link->uart_num, payload, hdr->len, pdMS_TO_TICKS(ms));
        if (n != hdr->len) return -1;
    }

    /* Read and verify CRC */
    uint8_t rx_crc;
    {
        int64_t remaining_us = deadline - esp_timer_get_time();
        int ms = (remaining_us > 0) ? (int)(remaining_us / 1000) : 1;
        if (ms < 1) ms = 1;

        int n = uart_read_bytes(link->uart_num, &rx_crc, 1, pdMS_TO_TICKS(ms));
        if (n != 1) return -1;
    }

    /* Compute expected CRC over DST+SRC+TYPE+LEN+PAYLOAD */
    uint8_t crc_buf[4 + PB_MAX_PAYLOAD];
    memcpy(crc_buf, hdr_rest, 4);
    if (hdr->len > 0) memcpy(&crc_buf[4], payload, hdr->len);
    uint8_t expected_crc = pb_crc8(crc_buf, 4 + hdr->len);

    if (rx_crc != expected_crc) {
        ESP_LOGW(TAG, "CRC mismatch: got 0x%02x, expected 0x%02x", rx_crc, expected_crc);
        return -1;
    }

    return hdr->len;
}

/* ------------------------------------------------------------------ */
/*  Sense GPIO                                                        */
/* ------------------------------------------------------------------ */

bool panel_bus_sense(const PbLink_t *link)
{
    return gpio_get_level(link->sense_gpio) != 0;
}

/* ------------------------------------------------------------------ */
/*  Discovery helpers                                                 */
/* ------------------------------------------------------------------ */

#define DISCOVER_TIMEOUT_MS     200
#define ASSIGN_TIMEOUT_MS       200

/* Send DISCOVER directly out a link (for controller's immediate neighbors) */
static int discover_direct(PbLink_t *link, PbTopology_t *topo, int8_t grid_x, int8_t grid_y)
{
    if (!panel_bus_sense(link)) {
        ESP_LOGI(TAG, "No neighbor on %s (sense low)",
                 link->side == PB_SIDE_NORTH ? "NORTH" : "EAST");
        return 0;
    }

    /* Flush any stale data in RX buffer */
    uart_flush_input(link->uart_num);

    /* Send DISCOVER with the side the neighbor is being probed from
     * (from their perspective, it's the opposite of our side) */
    uint8_t side_payload = PB_OPPOSITE_SIDE(link->side);
    if (pb_send_frame(link, PB_ADDR_UNASSIGNED, PB_ADDR_CONTROLLER,
                      PB_MSG_DISCOVER, &side_payload, 1) != 0) {
        return -1;
    }

    /* Wait for DISCOVER_RESP */
    PbFrameHeader_t resp_hdr;
    uint8_t resp_payload[PB_MAX_PAYLOAD];
    int resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                                 DISCOVER_TIMEOUT_MS);
    if (resp_len < 0 || resp_hdr.type != PB_MSG_DISCOVER_RESP || resp_len < 4) {
        ESP_LOGW(TAG, "No DISCOVER_RESP on %s",
                 link->side == PB_SIDE_NORTH ? "NORTH" : "EAST");
        return 0;
    }

    /* Check for duplicate hw_id (panel already discovered via another path) */
    for (int i = 0; i < topo->num_panels; i++) {
        if (memcmp(topo->panels[i].hw_id, resp_payload, 4) == 0) {
            ESP_LOGI(TAG, "Panel already discovered (hw_id match), skipping");
            return 0;
        }
    }

    /* Assign address */
    uint8_t assign_payload[5];
    memcpy(assign_payload, resp_payload, 4);        /* hw_id */
    assign_payload[4] = topo->next_addr;            /* new address */

    if (pb_send_frame(link, PB_ADDR_UNASSIGNED, PB_ADDR_CONTROLLER,
                      PB_MSG_ASSIGN_ADDR, assign_payload, 5) != 0) {
        return -1;
    }

    /* Wait for ASSIGN_RESP */
    resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                             ASSIGN_TIMEOUT_MS);
    if (resp_len < 0 || resp_hdr.type != PB_MSG_ASSIGN_RESP) {
        ESP_LOGW(TAG, "No ASSIGN_RESP");
        return -1;
    }

    /* Record panel info */
    if (topo->num_panels >= PB_MAX_PANELS) {
        ESP_LOGW(TAG, "Max panels reached");
        return 0;
    }

    PbPanelInfo_t *panel = &topo->panels[topo->num_panels];
    panel->addr = topo->next_addr++;
    memcpy(panel->hw_id, assign_payload, 4);
    panel->grid_x = grid_x;
    panel->grid_y = grid_y;
    panel->parent_side = PB_OPPOSITE_SIDE(link->side);
    panel->link = link;
    panel->online = true;
    panel->mux_in = PB_SIDE_NONE;
    panel->mux_out = PB_SIDE_NONE;

    /* Query neighbors */
    if (pb_send_frame(link, panel->addr, PB_ADDR_CONTROLLER,
                      PB_MSG_QUERY_NEIGHBORS, NULL, 0) == 0) {
        resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                                 DISCOVER_TIMEOUT_MS);
        if (resp_len >= 1 && resp_hdr.type == PB_MSG_NEIGHBORS_RESP) {
            panel->neighbors = resp_payload[0];
        }
    }

    topo->num_panels++;

    ESP_LOGI(TAG, "Discovered panel addr=%u at (%d,%d) hw_id=%02x%02x%02x%02x neighbors=0x%02x",
             panel->addr, panel->grid_x, panel->grid_y,
             panel->hw_id[0], panel->hw_id[1], panel->hw_id[2], panel->hw_id[3],
             panel->neighbors);

    return 1;
}

/* Send a FORWARD-wrapped message to a panel via its link.
 * The FORWARD payload is: [out_side: u8] [enclosed_frame: ...] */
static int send_forwarded(PbLink_t *link, uint8_t relay_addr,
                          PbSide_t out_side, uint8_t dst, uint8_t src,
                          uint8_t type, const uint8_t *payload, uint8_t len)
{
    /* Build the enclosed frame */
    uint8_t enclosed[PB_MAX_FRAME_SIZE];
    enclosed[0] = PB_SYNC_BYTE;
    enclosed[1] = dst;
    enclosed[2] = src;
    enclosed[3] = type;
    enclosed[4] = len;
    if (len > 0 && payload != NULL) {
        memcpy(&enclosed[5], payload, len);
    }
    enclosed[5 + len] = pb_crc8(&enclosed[1], 4 + len);
    uint8_t enclosed_len = PB_HEADER_SIZE + len + 1;

    /* Build FORWARD payload: [out_side] [enclosed_frame] */
    uint8_t fwd_payload[1 + PB_MAX_FRAME_SIZE];
    fwd_payload[0] = (uint8_t)out_side;
    memcpy(&fwd_payload[1], enclosed, enclosed_len);

    return pb_send_frame(link, relay_addr, PB_ADDR_CONTROLLER,
                         PB_MSG_FORWARD, fwd_payload, 1 + enclosed_len);
}

/* Discover a panel via forwarding through an already-addressed relay panel */
static int discover_forwarded(PbLink_t *link, uint8_t relay_addr,
                              PbSide_t out_side, PbTopology_t *topo,
                              int8_t grid_x, int8_t grid_y)
{
    uart_flush_input(link->uart_num);

    /* Send DISCOVER via FORWARD */
    uint8_t side_payload = PB_OPPOSITE_SIDE(out_side);
    if (send_forwarded(link, relay_addr, out_side,
                       PB_ADDR_UNASSIGNED, PB_ADDR_CONTROLLER,
                       PB_MSG_DISCOVER, &side_payload, 1) != 0) {
        return -1;
    }

    /* Wait for forwarded DISCOVER_RESP */
    PbFrameHeader_t resp_hdr;
    uint8_t resp_payload[PB_MAX_PAYLOAD];
    int resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                                 DISCOVER_TIMEOUT_MS * 2);
    if (resp_len < 0 || resp_hdr.type != PB_MSG_DISCOVER_RESP || resp_len < 4) {
        return 0; /* no panel there */
    }

    /* Check for duplicate */
    for (int i = 0; i < topo->num_panels; i++) {
        if (memcmp(topo->panels[i].hw_id, resp_payload, 4) == 0) {
            return 0;
        }
    }

    /* Assign address via FORWARD */
    uint8_t assign_payload[5];
    memcpy(assign_payload, resp_payload, 4);
    assign_payload[4] = topo->next_addr;

    if (send_forwarded(link, relay_addr, out_side,
                       PB_ADDR_UNASSIGNED, PB_ADDR_CONTROLLER,
                       PB_MSG_ASSIGN_ADDR, assign_payload, 5) != 0) {
        return -1;
    }

    resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                             ASSIGN_TIMEOUT_MS * 2);
    if (resp_len < 0 || resp_hdr.type != PB_MSG_ASSIGN_RESP) {
        return -1;
    }

    if (topo->num_panels >= PB_MAX_PANELS) return 0;

    PbPanelInfo_t *panel = &topo->panels[topo->num_panels];
    panel->addr = topo->next_addr++;
    memcpy(panel->hw_id, assign_payload, 4);
    panel->grid_x = grid_x;
    panel->grid_y = grid_y;
    panel->parent_side = PB_OPPOSITE_SIDE(out_side);
    panel->link = link;
    panel->online = true;
    panel->mux_in = PB_SIDE_NONE;
    panel->mux_out = PB_SIDE_NONE;

    /* Query neighbors via the relay chain — for simplicity, send directly
     * to the new panel's address (intermediate panels forward by address) */
    if (pb_send_frame(link, panel->addr, PB_ADDR_CONTROLLER,
                      PB_MSG_QUERY_NEIGHBORS, NULL, 0) == 0) {
        resp_len = pb_recv_frame(link, &resp_hdr, resp_payload, sizeof(resp_payload),
                                 DISCOVER_TIMEOUT_MS * 2);
        if (resp_len >= 1 && resp_hdr.type == PB_MSG_NEIGHBORS_RESP) {
            panel->neighbors = resp_payload[0];
        }
    }

    topo->num_panels++;

    ESP_LOGI(TAG, "Discovered panel addr=%u at (%d,%d) via relay addr=%u neighbors=0x%02x",
             panel->addr, panel->grid_x, panel->grid_y, relay_addr, panel->neighbors);

    return 1;
}

/* ------------------------------------------------------------------ */
/*  BFS Discovery                                                     */
/* ------------------------------------------------------------------ */

/* BFS queue entry */
typedef struct {
    uint8_t     panel_idx;      /* index into topo->panels[] */
    PbSide_t    side;           /* side to explore */
    int8_t      grid_x;        /* expected grid position of undiscovered panel */
    int8_t      grid_y;
} BfsEntry_t;

/* Grid position offset for each side */
static const int8_t side_dx[] = { 0, 1, 0, -1 };   /* N, E, S, W */
static const int8_t side_dy[] = { 1, 0, -1, 0 };

/* Check if a grid position is already occupied */
static bool grid_occupied(const PbTopology_t *topo, int8_t x, int8_t y)
{
    for (int i = 0; i < topo->num_panels; i++) {
        if (topo->panels[i].grid_x == x && topo->panels[i].grid_y == y)
            return true;
    }
    return false;
}

int panel_bus_discover(PbTopology_t *topo)
{
    memset(topo, 0, sizeof(*topo));
    topo->next_addr = 1;

    BfsEntry_t queue[PB_MAX_PANELS * 4];
    int queue_head = 0, queue_tail = 0;

    /* Step 1: discover direct neighbors of the controller */
    int found;

    /* Controller is at grid (0,0). NORTH neighbor is at (0,1). */
    found = discover_direct(&link_north, topo, 0, 1);
    if (found > 0) {
        PbPanelInfo_t *p = &topo->panels[topo->num_panels - 1];
        /* Enqueue unexplored sides of this panel */
        for (int s = 0; s < PB_SIDE_COUNT; s++) {
            if (s == (int)p->parent_side) continue;     /* skip side toward controller */
            if (!(p->neighbors & (1 << s))) continue;   /* skip sides with no neighbor */
            int8_t nx = p->grid_x + side_dx[s];
            int8_t ny = p->grid_y + side_dy[s];
            if (!grid_occupied(topo, nx, ny)) {
                queue[queue_tail++] = (BfsEntry_t){
                    .panel_idx = (uint8_t)(topo->num_panels - 1),
                    .side = (PbSide_t)s,
                    .grid_x = nx, .grid_y = ny,
                };
            }
        }
    }

    /* Controller's EAST neighbor is at (1,0). */
    found = discover_direct(&link_east, topo, 1, 0);
    if (found > 0) {
        PbPanelInfo_t *p = &topo->panels[topo->num_panels - 1];
        for (int s = 0; s < PB_SIDE_COUNT; s++) {
            if (s == (int)p->parent_side) continue;
            if (!(p->neighbors & (1 << s))) continue;
            int8_t nx = p->grid_x + side_dx[s];
            int8_t ny = p->grid_y + side_dy[s];
            if (!grid_occupied(topo, nx, ny)) {
                queue[queue_tail++] = (BfsEntry_t){
                    .panel_idx = (uint8_t)(topo->num_panels - 1),
                    .side = (PbSide_t)s,
                    .grid_x = nx, .grid_y = ny,
                };
            }
        }
    }

    /* Step 2: BFS through forwarded discovery */
    while (queue_head < queue_tail) {
        BfsEntry_t entry = queue[queue_head++];

        if (grid_occupied(topo, entry.grid_x, entry.grid_y)) continue;

        PbPanelInfo_t *relay = &topo->panels[entry.panel_idx];
        found = discover_forwarded(relay->link, relay->addr, entry.side,
                                   topo, entry.grid_x, entry.grid_y);
        if (found > 0) {
            PbPanelInfo_t *p = &topo->panels[topo->num_panels - 1];
            for (int s = 0; s < PB_SIDE_COUNT; s++) {
                if (s == (int)p->parent_side) continue;
                if (!(p->neighbors & (1 << s))) continue;
                int8_t nx = p->grid_x + side_dx[s];
                int8_t ny = p->grid_y + side_dy[s];
                if (!grid_occupied(topo, nx, ny)) {
                    queue[queue_tail++] = (BfsEntry_t){
                        .panel_idx = (uint8_t)(topo->num_panels - 1),
                        .side = (PbSide_t)s,
                        .grid_x = nx, .grid_y = ny,
                    };
                }
            }
        }
    }

    /* Compute grid bounds */
    topo->max_col = 0;
    topo->max_row = 0;
    for (int i = 0; i < topo->num_panels; i++) {
        if (topo->panels[i].grid_x > topo->max_col)
            topo->max_col = topo->panels[i].grid_x;
        if (topo->panels[i].grid_y > topo->max_row)
            topo->max_row = topo->panels[i].grid_y;
    }

    ESP_LOGI(TAG, "Discovery complete: %d panels, grid %dx%d",
             topo->num_panels, topo->max_col + 1, topo->max_row + 1);

    return topo->num_panels;
}

/* ------------------------------------------------------------------ */
/*  Snake path routing                                                */
/* ------------------------------------------------------------------ */

/* Find panel index at grid position, or -1 */
static int find_panel_at(const PbTopology_t *topo, int8_t x, int8_t y)
{
    for (int i = 0; i < topo->num_panels; i++) {
        if (topo->panels[i].grid_x == x && topo->panels[i].grid_y == y)
            return i;
    }
    return -1;
}

/* Determine which side of panel A connects to panel B based on grid positions */
static PbSide_t side_between(const PbPanelInfo_t *a, const PbPanelInfo_t *b)
{
    int8_t dx = b->grid_x - a->grid_x;
    int8_t dy = b->grid_y - a->grid_y;
    if (dx == 1 && dy == 0) return PB_SIDE_EAST;
    if (dx == -1 && dy == 0) return PB_SIDE_WEST;
    if (dx == 0 && dy == 1) return PB_SIDE_NORTH;
    if (dx == 0 && dy == -1) return PB_SIDE_SOUTH;
    return PB_SIDE_NONE;
}

int panel_bus_configure_routing(PbTopology_t *topo)
{
    if (topo->num_panels == 0) return 0;

    /* Build snake (boustrophedon) path through the grid.
     * Row 0 left-to-right, row 1 right-to-left, etc. */
    topo->chain_len = 0;

    for (int8_t row = 0; row <= topo->max_row; row++) {
        if (row % 2 == 0) {
            /* Left to right */
            for (int8_t col = 0; col <= topo->max_col; col++) {
                int idx = find_panel_at(topo, col, row);
                if (idx >= 0) {
                    topo->chain_order[topo->chain_len++] = (uint8_t)idx;
                }
            }
        } else {
            /* Right to left */
            for (int8_t col = topo->max_col; col >= 0; col--) {
                int idx = find_panel_at(topo, col, row);
                if (idx >= 0) {
                    topo->chain_order[topo->chain_len++] = (uint8_t)idx;
                }
            }
        }
    }

    /* Compute mux_in/mux_out for each panel in chain order */
    for (int i = 0; i < topo->chain_len; i++) {
        PbPanelInfo_t *panel = &topo->panels[topo->chain_order[i]];

        if (i == 0) {
            /* First panel receives PWM from controller.
             * Controller is at (0,0), so determine which side faces it. */
            if (panel->grid_x == 0 && panel->grid_y == 1) {
                panel->mux_in = PB_SIDE_SOUTH;  /* controller is to the south */
            } else if (panel->grid_x == 1 && panel->grid_y == 0) {
                panel->mux_in = PB_SIDE_WEST;   /* controller is to the west */
            } else {
                /* Shouldn't happen for a corner controller, but handle gracefully */
                panel->mux_in = panel->parent_side;
            }
        } else {
            PbPanelInfo_t *prev = &topo->panels[topo->chain_order[i - 1]];
            panel->mux_in = side_between(panel, prev);
        }

        if (i < topo->chain_len - 1) {
            PbPanelInfo_t *next = &topo->panels[topo->chain_order[i + 1]];
            panel->mux_out = side_between(panel, next);
        } else {
            panel->mux_out = PB_SIDE_NONE;  /* last panel in chain */
        }
    }

    /* Send SET_MUX to each panel */
    for (int i = 0; i < topo->chain_len; i++) {
        PbPanelInfo_t *panel = &topo->panels[topo->chain_order[i]];
        uint8_t mux_payload[2] = { panel->mux_in, panel->mux_out };

        if (pb_send_frame(panel->link, panel->addr, PB_ADDR_CONTROLLER,
                          PB_MSG_SET_MUX, mux_payload, 2) != 0) {
            ESP_LOGE(TAG, "Failed to send SET_MUX to addr=%u", panel->addr);
            return -1;
        }

        /* Wait for ack */
        PbFrameHeader_t resp_hdr;
        uint8_t resp_payload[8];
        int resp_len = pb_recv_frame(panel->link, &resp_hdr, resp_payload,
                                     sizeof(resp_payload), ASSIGN_TIMEOUT_MS);
        if (resp_len < 0 || resp_hdr.type != PB_MSG_SET_MUX_RESP) {
            ESP_LOGW(TAG, "No SET_MUX_RESP from addr=%u", panel->addr);
        }

        ESP_LOGI(TAG, "Panel addr=%u (%d,%d): mux_in=%d mux_out=%d",
                 panel->addr, panel->grid_x, panel->grid_y,
                 panel->mux_in, panel->mux_out);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Canvas rebuild                                                    */
/* ------------------------------------------------------------------ */

void panel_bus_rebuild_canvas(const PbTopology_t *topo)
{
    if (topo->chain_len == 0) {
        /* No panels discovered — use default single-panel canvas */
        canvas_init(&canvas);
        return;
    }

    canvas.num_panels = topo->chain_len;
    canvas.num_pixels = topo->chain_len * 64;

    /* Compute coordinate bounds */
    canvas.min_x = 0;
    canvas.min_y = 0;
    canvas.max_x = (topo->max_col + 1) * 8 - 1;
    canvas.max_y = (topo->max_row + 1) * 8 - 1;

    /* Build pixel mappings for each panel in chain order */
    int pixel_idx = 0;
    for (int p = 0; p < topo->chain_len; p++) {
        const PbPanelInfo_t *panel = &topo->panels[topo->chain_order[p]];
        int base_x = panel->grid_x * 8;
        int base_y = panel->grid_y * 8;
        int led_base = p * 64;

        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                /* Zigzag within panel: even rows L->R, odd rows R->L */
                int local_led = (y % 2 == 0) ? (y * 8 + x) : (y * 8 + (7 - x));

                canvas.pixels[pixel_idx].x = base_x + x;
                canvas.pixels[pixel_idx].y = base_y + y;
                canvas.pixels[pixel_idx].led_index = led_base + local_led;
                canvas.pixels[pixel_idx].panel_id = p;
                pixel_idx++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public topology access                                            */
/* ------------------------------------------------------------------ */

const PbTopology_t *panel_bus_get_topology(void)
{
    return &topology;
}

/* ------------------------------------------------------------------ */
/*  Panel bus task                                                    */
/* ------------------------------------------------------------------ */

void panel_bus_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Panel bus task started on core %d", xPortGetCoreID());

    /* Run initial discovery */
    int num_found = panel_bus_discover(&topology);

    if (num_found > 0) {
        /* Compute and apply mux routing */
        panel_bus_configure_routing(&topology);

        /* Rebuild canvas for multi-panel layout */
        canvas_updating = true;
        panel_bus_rebuild_canvas(&topology);
        canvas_updating = false;

        ESP_LOGI(TAG, "Panel grid configured: %d panels, %d LEDs in chain",
                 topology.chain_len, topology.chain_len * 64);
    } else {
        ESP_LOGI(TAG, "No panels discovered, using single-panel mode");
    }

    /* Main loop: listen for NEIGHBOR_CHANGE events on both links */
    PbFrameHeader_t hdr;
    uint8_t payload[PB_MAX_PAYLOAD];
    bool topology_dirty = false;
    int64_t dirty_since = 0;

    while (1) {
        /* Poll NORTH link */
        int len = pb_recv_frame(&link_north, &hdr, payload, sizeof(payload), 25);
        if (len >= 0 && hdr.type == PB_MSG_NEIGHBOR_CHANGE) {
            ESP_LOGI(TAG, "NEIGHBOR_CHANGE from addr=%u: side=%u present=%u",
                     hdr.src, payload[0], payload[1]);
            topology_dirty = true;
            dirty_since = esp_timer_get_time();
        }

        /* Poll EAST link */
        len = pb_recv_frame(&link_east, &hdr, payload, sizeof(payload), 25);
        if (len >= 0 && hdr.type == PB_MSG_NEIGHBOR_CHANGE) {
            ESP_LOGI(TAG, "NEIGHBOR_CHANGE from addr=%u: side=%u present=%u",
                     hdr.src, payload[0], payload[1]);
            topology_dirty = true;
            dirty_since = esp_timer_get_time();
        }

        /* Debounce: re-discover 500ms after last topology change */
        if (topology_dirty) {
            int64_t elapsed = esp_timer_get_time() - dirty_since;
            if (elapsed >= 500000) {    /* 500ms in microseconds */
                ESP_LOGI(TAG, "Topology changed, re-discovering...");

                /* Reset all panels */
                for (int i = 0; i < topology.num_panels; i++) {
                    pb_send_frame(topology.panels[i].link,
                                  topology.panels[i].addr, PB_ADDR_CONTROLLER,
                                  PB_MSG_RESET, NULL, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(50));

                /* Full re-discovery */
                num_found = panel_bus_discover(&topology);
                if (num_found > 0) {
                    panel_bus_configure_routing(&topology);
                    canvas_updating = true;
                    panel_bus_rebuild_canvas(&topology);
                    canvas_updating = false;
                } else {
                    canvas_updating = true;
                    canvas_init(&canvas);
                    canvas_updating = false;
                }

                topology_dirty = false;
            }
        }
    }
}
