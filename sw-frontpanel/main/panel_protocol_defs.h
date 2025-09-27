#pragma once

#include <stdint.h>

// Panel communication protocol definitions
// Shared between ESP32 front panel and RP2350 main board

// Protocol constants
#define PANEL_PROTOCOL_HEADER_SIZE     5
#define PANEL_PROTOCOL_MAX_PAYLOAD     4096

// Command bit masks
#define PANEL_CMD_DIR_WRITE           0x00
#define PANEL_CMD_DIR_READ            0x80
#define PANEL_CMD_DIR_MASK            0x80
#define PANEL_CMD_ASYNC_FLAG          0x40  // Bit 6 indicates async operation

// Write commands (bit 7 = 0)
#define PANEL_CMD_START_DISC_COUNT     0x42  // Start disc enumeration (async)
#define PANEL_CMD_SELECT_DISC          0x44  // Select disc (async, arg: index or 0xFF for extended)
#define PANEL_CMD_EJECT_DISC           0x45  // Eject current disc (async)
#define PANEL_CMD_START_DISC_INFO      0x46  // Start disc info read (async, arg: index or 0xFF for extended)
#define PANEL_CMD_CHECK_FIRMWARE       0x50  // Check for firmware update (async)
#define PANEL_CMD_START_FIRMWARE_READ  0x51  // Start firmware read (async, arg: chunk index)
#define PANEL_CMD_START_FILE_UPLOAD    0x52  // Start file upload (async, payload: file_upload_start_t)
#define PANEL_CMD_WRITE_FILE_CHUNK     0x53  // Write file chunk (async, arg: chunk crc16, payload: chunk data)
#define PANEL_CMD_FINISH_FILE_UPLOAD   0x54  // Finish file upload (async)
#define PANEL_CMD_RESET                0x7F  // Reset system

// Read commands (bit 7 = 1)
#define PANEL_CMD_POLL_STATUS          0x80  // Get general status (1 byte response)
#define PANEL_CMD_POLL_OP_READY        0x81  // Check if async operation ready (3 bytes: ready, size low, size high)
#define PANEL_CMD_GET_DISC_STATUS      0x83  // Get disc loaded status (1 byte response)
#define PANEL_CMD_GET_FIRMWARE_INFO    0x84  // Get firmware info after CHECK_FIRMWARE (16 bytes)
#define PANEL_CMD_GET_PLAYBACK_STATUS  0x85  // Get current playback status (panel_playback_status_t)

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
#define PANEL_ARG_EXTENDED             0xFFFF  // Use payload for extended data
#define PANEL_ARG_IGNORED              0x0000  // Argument not used

// Protocol header structure (5 bytes)
typedef struct __attribute__((packed)) {
    uint8_t command;       // Command code with direction bit
    uint16_t argument;     // Optional argument halfword
    uint16_t payload_size; // Size of phase 2 transfer (little-endian)
} panel_protocol_header_t;

// Status response structure for POLL_OP_READY (3 bytes)
typedef struct __attribute__((packed)) {
    uint8_t ready_flag;    // 1 if ready, 0 if not
    uint16_t response_size; // Size of result data (little-endian)
} panel_status_response_t;

// Helper macros
#define PANEL_CMD_IS_READ(cmd)  ((cmd) & PANEL_CMD_DIR_MASK)
#define PANEL_CMD_IS_WRITE(cmd) (!PANEL_CMD_IS_READ(cmd))
#define PANEL_CMD_IS_ASYNC(cmd) ((cmd) & PANEL_CMD_ASYNC_FLAG)

// Async operation states (internal to RP2350)
typedef enum {
    PANEL_ASYNC_IDLE = 0,
    PANEL_ASYNC_PROCESSING,
    PANEL_ASYNC_READY,
    PANEL_ASYNC_ERROR
} panel_async_state_t;

// Firmware info structure (45 bytes)
typedef struct __attribute__((packed)) {
    uint32_t size;         // Firmware size in bytes
    uint32_t version;      // Version number (major.minor.patch as 0xMMmmpppp)
    uint8_t sha256[32];    // SHA256 hash (32 bytes)
    uint8_t available;     // 1 if update available, 0 if not
    uint8_t reserved[8];   // Reserved for alignment
} panel_firmware_info_t;

// Firmware chunk size (must fit within PANEL_PROTOCOL_MAX_PAYLOAD)
#define PANEL_FIRMWARE_CHUNK_SIZE PANEL_PROTOCOL_MAX_PAYLOAD

// File upload chunk size (must fit within PANEL_PROTOCOL_MAX_PAYLOAD)
#define PANEL_FILE_CHUNK_SIZE PANEL_PROTOCOL_MAX_PAYLOAD

// File upload start structure (variable length with filename and hash)
typedef struct __attribute__((packed)) {
    uint32_t file_size;        // Total file size in bytes
    uint16_t filename_len;     // Length of filename string
    // Filename follows (null-terminated string)
} panel_file_upload_start_t;

// File upload result codes
#define PANEL_UPLOAD_OK            0x00
#define PANEL_UPLOAD_ERROR_DISK    0x01
#define PANEL_UPLOAD_ERROR_SPACE   0x02
#define PANEL_UPLOAD_ERROR_WRITE   0x03
#define PANEL_UPLOAD_ERROR_PATH    0x04

// Disc type codes for playback status
#define PANEL_DISC_TYPE_NO_DISC    0x00
#define PANEL_DISC_TYPE_DATA       0x01
#define PANEL_DISC_TYPE_AUDIO      0x02
#define PANEL_DISC_TYPE_MIXED      0x03

// Audio playback status codes (matches CDRomAudioStatus)
#define PANEL_AUDIO_STATUS_DATA_ONLY          0x00
#define PANEL_AUDIO_STATUS_PLAYING            0x11
#define PANEL_AUDIO_STATUS_PAUSED             0x12
#define PANEL_AUDIO_STATUS_PLAYING_COMPLETED  0x13
#define PANEL_AUDIO_STATUS_PLAY_ERROR         0x14
#define PANEL_AUDIO_STATUS_NONE               0x15

// Playback status structure (76 bytes)
typedef struct __attribute__((packed)) {
    uint8_t disc_inserted;    // 1 if disc loaded, 0 if not
    uint8_t disc_type;        // PANEL_DISC_TYPE_*
    uint8_t is_playing;       // 1 if currently playing audio, 0 if not
    uint8_t audio_status;     // PANEL_AUDIO_STATUS_*
    uint8_t current_track;    // Current track number (1-99)
    uint8_t track_position_m; // Track position: minutes
    uint8_t track_position_s; // Track position: seconds
    uint8_t track_position_f; // Track position: frames
    char disc_name[64];       // Current disc name (null-terminated)
    uint8_t reserved[4];      // Reserved for future use
} panel_playback_status_t;
