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

// Write commands (bit 7 = 0, bit 6 = 1 for async)
#define PANEL_CMD_GET_DIR_ENTRY_COUNT  0x42  // Get count of entries in current directory (async)
#define PANEL_CMD_GET_ENTRY_INFO       0x43  // Get info for entry at index (async, arg: index)
#define PANEL_CMD_SELECT_ENTRY         0x44  // Select entry by index: -1 (0xFFFF)=parent dir, >=0=select entry (async, arg: signed 16-bit index)
#define PANEL_CMD_GET_CURRENT_PATH     0x45  // Get current directory path (async)
#define PANEL_CMD_EJECT_IMAGE          0x47  // Unload current image (async)
#define PANEL_CMD_GET_LOADED_IMAGE_STATUS 0x48  // Get status of currently loaded image (async)
#define PANEL_CMD_SELECT_PREV_IMAGE    0x49  // Load previous image in current directory (async)
#define PANEL_CMD_SELECT_NEXT_IMAGE    0x4A  // Load next image in current directory (async)
#define PANEL_CMD_SELECT_IMAGE_BY_NAME 0x4B  // Load image by filename (async, payload: null-terminated filename)
#define PANEL_CMD_CHECK_FIRMWARE       0x50  // Check for firmware update (async)
#define PANEL_CMD_START_FIRMWARE_READ  0x51  // Start firmware read (async, arg: chunk index)
#define PANEL_CMD_START_FILE_UPLOAD    0x52  // Start file upload (async, payload: file_upload_start_t)
#define PANEL_CMD_WRITE_FILE_CHUNK     0x53  // Write file chunk (async, arg: chunk crc16, payload: chunk data)
#define PANEL_CMD_FINISH_FILE_UPLOAD   0x54  // Finish file upload (async)
#define PANEL_CMD_GET_RP2350_FW_STATUS 0x55  // Get RP2350 firmware status (async, returns rp2350_fw_status_t)
#define PANEL_CMD_START_RP2350_UPDATE  0x56  // Start RP2350 firmware update from SD card (async, reboots on success)
#define PANEL_CMD_RESET                0x7F  // Reset system

// Read commands (bit 7 = 1)
#define PANEL_CMD_POLL_STATUS          0x80  // Get general status (1 byte response)
#define PANEL_CMD_POLL_OP_READY        0x81  // Check if async operation ready (3 bytes: ready, size low, size high)
#define PANEL_CMD_GET_DEVICE_STATUS    0x83  // Get device status (1 byte response)
#define PANEL_CMD_GET_FIRMWARE_INFO    0x84  // Get firmware info after CHECK_FIRMWARE (45 bytes)
#define PANEL_CMD_GET_PLAYBACK_STATUS  0x85  // Get current playback status (panel_playback_status_t)
#define PANEL_CMD_GET_COMMAND_STATUS   0x86  // Get detailed async command status (panel_command_status_t)

// Status codes for POLL_STATUS
#define PANEL_STATUS_OK                0x00  // System OK
#define PANEL_STATUS_BUSY              0x01  // Operation in progress
#define PANEL_STATUS_ERROR             0x02  // Last operation failed
#define PANEL_STATUS_NO_OPERATION      0x03  // No operation pending

// Device status codes for GET_DEVICE_STATUS
#define PANEL_DEVICE_STATUS_NO_IMAGE   0x00  // No image loaded
#define PANEL_DEVICE_STATUS_LOADED     0x01  // Image loaded and ready
#define PANEL_DEVICE_STATUS_LOADING    0x02  // Image loading in progress
#define PANEL_DEVICE_STATUS_ERROR      0x03  // Image error

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

// Detailed command status for GET_COMMAND_STATUS (4 bytes)
typedef struct __attribute__((packed)) {
    uint8_t command;       // Current/last async command
    uint8_t state;         // PANEL_ASYNC_* state
    uint8_t progress;      // 0-100 progress (command-specific)
    uint8_t last_result;   // Result of last completed operation
} panel_command_status_t;

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
    uint32_t version;      // Version number (major.minor.patch as 0x00MMmmpp)
    uint8_t sha256[32];    // SHA256 hash (32 bytes)
    uint8_t available;     // 1 if update available, 0 if not
    uint8_t reserved[8];   // Reserved for alignment
} panel_firmware_info_t;

// RP2350 firmware status structure
typedef struct __attribute__((packed)) {
    uint32_t current_version;      // Running version (0x00MMmmpp)
    uint32_t available_version;    // Available update version (0 if none)
    uint8_t update_progress;       // Update progress (0-100), 0 if not updating
    uint8_t last_update_result;    // Result of last update attempt
} rp2350_fw_status_t;

// Firmware chunk size (must fit within PANEL_PROTOCOL_MAX_PAYLOAD)
#define PANEL_FIRMWARE_CHUNK_SIZE  PANEL_PROTOCOL_MAX_PAYLOAD

// File upload chunk size (must fit within PANEL_PROTOCOL_MAX_PAYLOAD)
#define PANEL_FILE_CHUNK_SIZE      PANEL_PROTOCOL_MAX_PAYLOAD

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

// Entry type for directory listings
#define PANEL_ENTRY_TYPE_DIRECTORY  0x00  // Subdirectory (navigate into it)
#define PANEL_ENTRY_TYPE_FILE       0x01  // Image file (load it)

// Directory entry information structure (68 bytes)
typedef struct __attribute__((packed)) {
    char name[64];           // Filename or directory name (null-terminated)
    uint8_t entry_type;      // PANEL_ENTRY_TYPE_*
    uint8_t reserved[3];     // Padding for alignment
} dir_entry_info_t;

// Device type codes
#define PANEL_DEVICE_TYPE_ATAPI  0x00  // CD-ROM drive
#define PANEL_DEVICE_TYPE_IDE    0x01  // Hard disk drive

// Currently loaded image status structure (212 bytes)
typedef struct __attribute__((packed)) {
    uint8_t image_loaded;         // 1 if image loaded, 0 if not
    uint8_t device_type;          // PANEL_DEVICE_TYPE_*
    uint8_t reserved1[2];         // Padding
    char image_name[64];          // Name of loaded image (null-terminated)
    char directory_path[128];     // Directory containing the image (null-terminated)
    uint32_t image_index;         // Index in current directory (0-based, only files counted)
    uint32_t total_images;        // Total number of images in directory
    // IDE-specific (only when device_type == PANEL_DEVICE_TYPE_IDE)
    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors;
    uint8_t reserved2[8];         // Reserved for future use
} loaded_image_status_t;
