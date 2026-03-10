#ifndef PANEL_BUS_H
#define PANEL_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  UART / GPIO configuration (controller v2 board)                   */
/*  Controller sits at bottom-left corner: NORTH and EAST links       */
/* ------------------------------------------------------------------ */

#define PB_NORTH_UART_NUM       1
#define PB_NORTH_TX_GPIO        39
#define PB_NORTH_RX_GPIO        40
#define PB_NORTH_SENSE_GPIO     14

#define PB_EAST_UART_NUM        2
#define PB_EAST_TX_GPIO         47
#define PB_EAST_RX_GPIO         48
#define PB_EAST_SENSE_GPIO      42

#define PB_MUX_SELECT_GPIO      10
#define PB_STATUS_LED_GPIO      41

#define PB_BAUD_RATE            115200
#define PB_UART_RX_BUF_SIZE     256

/* ------------------------------------------------------------------ */
/*  Addresses                                                         */
/*                                                                    */
/*  XY dimension-order routing: addresses encode grid coordinates.     */
/*  High nibble = x (column), low nibble = y (row).                   */
/*  Controller at (0,0) = 0x00. Panel at (2,3) = 0x23.               */
/*  Panels route by comparing dst coords to own coords:               */
/*    route X-axis first (EAST/WEST), then Y-axis (NORTH/SOUTH).      */
/* ------------------------------------------------------------------ */

#define PB_ADDR_XY(x, y)       (((uint8_t)(x) << 4) | ((uint8_t)(y) & 0x0F))
#define PB_ADDR_X(addr)        (((uint8_t)(addr) >> 4) & 0x0F)
#define PB_ADDR_Y(addr)        ((uint8_t)(addr) & 0x0F)

#define PB_ADDR_CONTROLLER      0x00    /* (0,0) */
#define PB_ADDR_UNASSIGNED      0xFE
#define PB_ADDR_BROADCAST       0xFF

#define PB_MAX_PANELS           64

/* ------------------------------------------------------------------ */
/*  Sides                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    PB_SIDE_NORTH = 0,
    PB_SIDE_EAST  = 1,
    PB_SIDE_SOUTH = 2,
    PB_SIDE_WEST  = 3,
    PB_SIDE_COUNT = 4,
    PB_SIDE_NONE  = 0xFF,
} PbSide_t;

/* Bitmask helpers for neighbor presence */
#define PB_SENSE_NORTH  (1 << PB_SIDE_NORTH)
#define PB_SENSE_EAST   (1 << PB_SIDE_EAST)
#define PB_SENSE_SOUTH  (1 << PB_SIDE_SOUTH)
#define PB_SENSE_WEST   (1 << PB_SIDE_WEST)

/* Opposite side lookup */
#define PB_OPPOSITE_SIDE(s) ((PbSide_t)(((s) + 2) % PB_SIDE_COUNT))

/* ------------------------------------------------------------------ */
/*  Frame format                                                      */
/*                                                                    */
/*  [SYNC: 0xAA] [DST: u8] [SRC: u8] [TYPE: u8] [LEN: u8]           */
/*  [PAYLOAD: 0..LEN bytes] [CRC8: u8]                                */
/*                                                                    */
/*  CRC-8 (poly 0x07) over DST through end of payload.               */
/*  Total overhead: 6 bytes. Max payload: 128 bytes.                  */
/* ------------------------------------------------------------------ */

#define PB_SYNC_BYTE        0xAA
#define PB_HEADER_SIZE      5       /* SYNC + DST + SRC + TYPE + LEN */
#define PB_MAX_PAYLOAD      128
#define PB_MAX_FRAME_SIZE   (PB_HEADER_SIZE + PB_MAX_PAYLOAD + 1)

typedef struct __attribute__((packed)) {
    uint8_t sync;
    uint8_t dst;
    uint8_t src;
    uint8_t type;
    uint8_t len;
} PbFrameHeader_t;

/* ------------------------------------------------------------------ */
/*  Message types                                                     */
/*                                                                    */
/*  Controller -> Panel: 0x01-0x3F                                    */
/*  Panel -> Controller: 0x81-0xBF                                    */
/*  Either direction:    0xC0-0xDF                                    */
/* ------------------------------------------------------------------ */

/* --- Discovery --- */
#define PB_MSG_DISCOVER             0x01    /* C->P  [side: u8] */
#define PB_MSG_ASSIGN_ADDR          0x02    /* C->P  [hw_id: 4B] [addr: u8] (addr is XY-encoded) */
#define PB_MSG_QUERY_NEIGHBORS      0x03    /* C->P  (empty) */
#define PB_MSG_DISCOVER_SIDE        0x04    /* C->P  [side: u8] [new_addr: u8] — proxy discovery */

/* --- Mux control --- */
#define PB_MSG_SET_MUX              0x10    /* C->P  [pwm_in: u8] [pwm_out: u8] */

/* --- General control --- */
#define PB_MSG_RESET                0x20    /* C->P  (empty) — revert to unassigned */

/* --- Panel -> Controller responses --- */
#define PB_MSG_DISCOVER_RESP        0x81    /* P->C  [hw_id: 4B] */
#define PB_MSG_ASSIGN_RESP          0x82    /* P->C  [addr: u8] */
#define PB_MSG_NEIGHBORS_RESP       0x83    /* P->C  [presence: u8 bitmask] */
#define PB_MSG_SET_MUX_RESP         0x84    /* P->C  [status: u8] */
#define PB_MSG_DISCOVER_SIDE_RESP   0x85    /* P->C  [hw_id: 4B] [neighbors: u8] */
#define PB_MSG_NEIGHBOR_CHANGE      0x86    /* P->C  [side: u8] [present: u8] (unsolicited) */

/* --- Either direction --- */
#define PB_MSG_PING                 0xC0    /* [seq: u8] */
#define PB_MSG_PONG                 0xC1    /* [seq: u8] */

/* ------------------------------------------------------------------ */
/*  Panel link handle (one per UART connection on controller)          */
/* ------------------------------------------------------------------ */

typedef struct {
    int         uart_num;
    PbSide_t    side;           /* which side of the controller this link represents */
    int         sense_gpio;
} PbLink_t;

/* ------------------------------------------------------------------ */
/*  Discovered panel info                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t     addr;           /* XY-encoded address: (x << 4) | y */
    uint8_t     hw_id[4];       /* STM32 UID (truncated to 4 bytes) */
    int8_t      grid_x;         /* column in grid (0 = controller column) */
    int8_t      grid_y;         /* row in grid (0 = controller row) */
    uint8_t     neighbors;      /* presence bitmask from NEIGHBORS_RESP */
    uint8_t     mux_in;         /* configured PWM input side */
    uint8_t     mux_out;        /* configured PWM output side */
    PbSide_t    parent_side;    /* side toward controller in spanning tree */
    bool        online;
} PbPanelInfo_t;

/* ------------------------------------------------------------------ */
/*  Topology state                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    PbPanelInfo_t   panels[PB_MAX_PANELS];
    uint8_t         num_panels;

    /* Snake path order for WS2812B daisy chain */
    uint8_t         chain_order[PB_MAX_PANELS]; /* indices into panels[] */
    uint8_t         chain_len;

    /* Grid bounds (computed after discovery) */
    int8_t          max_col;
    int8_t          max_row;
} PbTopology_t;

/* ------------------------------------------------------------------ */
/*  CRC-8                                                             */
/* ------------------------------------------------------------------ */

uint8_t pb_crc8(const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/*  Frame I/O                                                         */
/* ------------------------------------------------------------------ */

/* Send a frame on a specific link. Returns 0 on success, -1 on error. */
int pb_send_frame(const PbLink_t *link, uint8_t dst, uint8_t src,
                  uint8_t type, const uint8_t *payload, uint8_t len);

/* Receive a frame from a link. Blocks up to timeout_ms.
 * Returns payload length on success, -1 on timeout/error. */
int pb_recv_frame(const PbLink_t *link, PbFrameHeader_t *hdr,
                  uint8_t *payload, uint8_t max_len, int timeout_ms);

/* ------------------------------------------------------------------ */
/*  High-level API                                                    */
/* ------------------------------------------------------------------ */

/* Initialize UARTs and sense GPIOs */
void panel_bus_init(void);

/* Run full BFS discovery. Returns number of panels found. */
int panel_bus_discover(PbTopology_t *topo);

/* Compute snake path and send SET_MUX to all panels. Returns 0 on success. */
int panel_bus_configure_routing(PbTopology_t *topo);

/* Rebuild canvas from discovered topology */
void panel_bus_rebuild_canvas(const PbTopology_t *topo);

/* Check if a side has a neighbor connected (reads sense GPIO) */
bool panel_bus_sense(const PbLink_t *link);

/* Set the controller's hardware MUX to route WS2812B signal out a side */
void panel_bus_set_controller_mux(PbSide_t out_side);

/* Get the global topology (read-only access for console/status) */
const PbTopology_t *panel_bus_get_topology(void);

/* FreeRTOS task function */
void panel_bus_task(void *pvParameters);

#endif /* PANEL_BUS_H */
