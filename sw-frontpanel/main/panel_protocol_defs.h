#pragma once

#include <stdint.h>

// Panel communication protocol definitions
// Shared between ESP32 front panel and RP2350 main board

// Protocol constants
#define PANEL_PROTOCOL_HEADER_SIZE     4
#define PANEL_PROTOCOL_MAX_PAYLOAD     256

// Command direction bit mask
#define PANEL_CMD_DIR_WRITE           0x00
#define PANEL_CMD_DIR_READ            0x80
#define PANEL_CMD_DIR_MASK            0x80

// Write commands (bit 7 = 0)
#define PANEL_CMD_START_DISC_COUNT     0x02  // Start disc enumeration
#define PANEL_CMD_SELECT_DISC          0x04  // Select disc (arg: index or 0xFF for extended)
#define PANEL_CMD_EJECT_DISC           0x05  // Eject current disc
#define PANEL_CMD_START_DISC_INFO      0x06  // Start disc info read (arg: index or 0xFF for extended)
#define PANEL_CMD_RESET                0x7F  // Reset system

// Read commands (bit 7 = 1)
#define PANEL_CMD_POLL_STATUS          0x80  // Get general status (1 byte response)
#define PANEL_CMD_POLL_OP_READY        0x81  // Check if async operation ready (2 bytes: ready, size)
#define PANEL_CMD_GET_OP_RESULT        0x82  // Get async operation result (variable size)
#define PANEL_CMD_GET_DISC_STATUS      0x83  // Get disc loaded status (1 byte response)

// Status codes for POLL_STATUS
#define PANEL_STATUS_OK                0x00  // System OK
#define PANEL_STATUS_BUSY              0x01  // Operation in progress
#define PANEL_STATUS_ERROR             0x02  // Last operation failed
#define PANEL_STATUS_NO_OPERATION      0x03  // No operation pending

// Disc status codes for GET_DISC_STATUS
#define PANEL_DISC_STATUS_NO_DISC      0x00  // No disc loaded
#define PANEL_DISC_STATUS_LOADED       0x01  // Disc loaded and ready
#define PANEL_DISC_STATUS_LOADING      0x02  // Disc loading in progress
#define PANEL_DISC_STATUS_ERROR        0x03  // Disc error

// Special argument values
#define PANEL_ARG_EXTENDED             0xFF  // Use payload for extended data
#define PANEL_ARG_IGNORED              0x00  // Argument not used

// Protocol header structure (4 bytes)
typedef struct __attribute__((packed)) {
    uint8_t command;      // Command code with direction bit
    uint8_t argument;     // Optional argument byte
    uint16_t payload_size; // Size of phase 2 transfer (little-endian)
} panel_protocol_header_t;

// Helper macros
#define PANEL_CMD_IS_READ(cmd)  ((cmd) & PANEL_CMD_DIR_MASK)
#define PANEL_CMD_IS_WRITE(cmd) (!PANEL_CMD_IS_READ(cmd))

// Async operation states (internal to RP2350)
typedef enum {
    PANEL_ASYNC_IDLE = 0,
    PANEL_ASYNC_PROCESSING,
    PANEL_ASYNC_READY,
    PANEL_ASYNC_ERROR
} panel_async_state_t;