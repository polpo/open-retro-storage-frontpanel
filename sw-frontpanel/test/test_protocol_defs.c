// SPDX-License-Identifier: GPL-2.0-only
//
//  Tier 1: contract tests for panel_protocol_defs.h.
//
//  This header is the wire contract shared with the RP2350 main board. These
//  tests pin down the things a careless edit could silently break: struct
//  sizes (which the header documents in comments), field packing/offsets,
//  byte-order of the on-wire header, command-code direction/async bits, and
//  the helper macros. Nothing here touches hardware.

#include "test_framework.h"
#include "panel_protocol_defs.h"
#include "panel_protocol_defs_initiator.h"
#include <stddef.h>

// --- Struct sizes (must match the sizes the header comments promise, because
//     the main board memcpy's these structures byte-for-byte) ---------------

TEST(protocol_struct_sizes) {
    CHECK_EQ_INT(sizeof(panel_protocol_header_t), 5);
    CHECK_EQ_INT(PANEL_PROTOCOL_HEADER_SIZE, 5);
    CHECK_EQ_INT(sizeof(panel_status_response_t), 3);
    CHECK_EQ_INT(sizeof(panel_command_status_t), 4);
    // NOTE: the header comment says "45 bytes" but the real packed layout is
    // 4+4+32+1+8 = 49. sizeof() is the authoritative ABI (both sides compile
    // the same struct); the prose comment is stale. Pin the real size.
    CHECK_EQ_INT(sizeof(panel_firmware_info_t), 49);
    CHECK_EQ_INT(sizeof(panel_playback_status_t), 76);
    CHECK_EQ_INT(sizeof(dir_entry_info_t), 68);
    CHECK_EQ_INT(sizeof(device_summary_t), 68);
    // NOTE: header comment says "212 bytes"; real packed size is 216
    // (reserved2[8] tail). Stale comment; pin the real size.
    CHECK_EQ_INT(sizeof(loaded_image_status_t), 216);
    CHECK_EQ_INT(sizeof(rp2350_fw_status_t), 10);
    CHECK_EQ_INT(sizeof(panel_file_upload_start_t), 6);
    CHECK_EQ_INT(sizeof(panel_file_download_start_result_t), 5);
}

// --- Packing: structs must be byte-packed (no padding) so offsets match the
//     main board's view of the same bytes -------------------------------------

TEST(protocol_header_is_packed) {
    // command(1) + argument(2) + payload_size(2), no padding.
    CHECK_EQ_INT(offsetof(panel_protocol_header_t, command), 0);
    CHECK_EQ_INT(offsetof(panel_protocol_header_t, argument), 1);
    CHECK_EQ_INT(offsetof(panel_protocol_header_t, payload_size), 3);
}

TEST(protocol_playback_status_offsets) {
    CHECK_EQ_INT(offsetof(panel_playback_status_t, disc_inserted), 0);
    CHECK_EQ_INT(offsetof(panel_playback_status_t, disc_name), 8);
    CHECK_EQ_INT(offsetof(panel_playback_status_t, device_status), 72);
    CHECK_EQ_INT(offsetof(panel_playback_status_t, alive_magic), 73);
    CHECK_EQ_INT(offsetof(panel_playback_status_t, tray_open), 74);
}

TEST(protocol_loaded_image_status_offsets) {
    CHECK_EQ_INT(offsetof(loaded_image_status_t, image_loaded), 0);
    CHECK_EQ_INT(offsetof(loaded_image_status_t, image_name), 4);
    CHECK_EQ_INT(offsetof(loaded_image_status_t, directory_path), 68);
    CHECK_EQ_INT(offsetof(loaded_image_status_t, image_index), 196);
    CHECK_EQ_INT(offsetof(loaded_image_status_t, total_images), 200);
    CHECK_EQ_INT(offsetof(loaded_image_status_t, cylinders), 204);
}

TEST(protocol_device_summary_offsets) {
    CHECK_EQ_INT(offsetof(device_summary_t, device_index), 0);
    CHECK_EQ_INT(offsetof(device_summary_t, device_type), 2);
    CHECK_EQ_INT(offsetof(device_summary_t, device_status), 3);
    CHECK_EQ_INT(offsetof(device_summary_t, device_label), 4);
    CHECK_EQ_INT(offsetof(device_summary_t, image_name), 36);
    // Flexible-array list begins right after the 4-byte fixed header.
    CHECK_EQ_INT(offsetof(device_list_response_t, devices), 4);
}

// --- On-wire header byte layout: little-endian argument and payload_size. ----

TEST(protocol_header_wire_layout) {
    panel_protocol_header_t h = {
        .command = PANEL_CMD_GET_ENTRY_INFO,
        .argument = 0x1234,
        .payload_size = 0x00AB,
    };
    const uint8_t *bytes = (const uint8_t *)&h;
    CHECK_EQ_HEX(bytes[0], PANEL_CMD_GET_ENTRY_INFO); // command
    CHECK_EQ_HEX(bytes[1], 0x34);                     // argument LSB
    CHECK_EQ_HEX(bytes[2], 0x12);                     // argument MSB
    CHECK_EQ_HEX(bytes[3], 0xAB);                     // payload_size LSB
    CHECK_EQ_HEX(bytes[4], 0x00);                     // payload_size MSB
}

// --- Command code direction/async bits. The high bit is direction (read=1),
//     bit 6 is the async flag. These are part of the contract: the main board
//     dispatches on them. -------------------------------------------------------

TEST(protocol_write_commands_have_clear_direction_bit) {
    // Async write commands: direction bit clear (0x00), async bit set (0x40).
    const uint8_t writes[] = {
        PANEL_CMD_GET_DIR_ENTRY_COUNT, PANEL_CMD_GET_ENTRY_INFO,
        PANEL_CMD_SELECT_ENTRY, PANEL_CMD_GET_CURRENT_PATH,
        PANEL_CMD_GET_DEVICE_LIST, PANEL_CMD_EJECT_IMAGE,
        PANEL_CMD_GET_LOADED_IMAGE_STATUS, PANEL_CMD_SELECT_PREV_IMAGE,
        PANEL_CMD_SELECT_NEXT_IMAGE, PANEL_CMD_SELECT_IMAGE_BY_NAME,
        PANEL_CMD_CHECK_FIRMWARE, PANEL_CMD_START_FIRMWARE_READ,
        PANEL_CMD_START_FILE_UPLOAD, PANEL_CMD_WRITE_FILE_CHUNK,
        PANEL_CMD_FINISH_FILE_UPLOAD, PANEL_CMD_GET_RP2350_FW_STATUS,
        PANEL_CMD_START_RP2350_UPDATE, PANEL_CMD_START_FILE_DOWNLOAD,
        PANEL_CMD_READ_FILE_CHUNK,
        PANEL_CMD_DELETE_FILE, PANEL_CMD_RENAME_FILE,
        PANEL_CMD_TOUCH_FILE, PANEL_CMD_MKDIR,
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        CHECK_TRUE(PANEL_CMD_IS_WRITE(writes[i]));
        CHECK_TRUE(PANEL_CMD_IS_ASYNC(writes[i]));
    }
}

TEST(protocol_delete_rename_opcode_values) {
    // These opcodes are part of the wire contract with the main board and must
    // not collide with existing commands (0x59 is GET_INITIATOR_STATUS there).
    CHECK_EQ_HEX(PANEL_CMD_DELETE_FILE, 0x5A);
    CHECK_EQ_HEX(PANEL_CMD_RENAME_FILE, 0x5B);
    CHECK_EQ_HEX(PANEL_CMD_TOUCH_FILE, 0x5C);
    CHECK_EQ_HEX(PANEL_CMD_MKDIR, 0x5D);
    // Result codes the host_comm layer relies on.
    CHECK_EQ_HEX(PANEL_DELETE_OK, 0x00);
    CHECK_EQ_HEX(PANEL_RENAME_OK, 0x00);
    CHECK_EQ_HEX(PANEL_DELETE_ERROR_IN_USE, 0x02);
    CHECK_EQ_HEX(PANEL_RENAME_ERROR_IN_USE, 0x03);
}

TEST(protocol_read_commands_have_set_direction_bit) {
    const uint8_t reads[] = {
        PANEL_CMD_POLL_STATUS, PANEL_CMD_POLL_OP_READY,
        PANEL_CMD_GET_DEVICE_STATUS, PANEL_CMD_GET_FIRMWARE_INFO,
        PANEL_CMD_GET_PLAYBACK_STATUS, PANEL_CMD_GET_COMMAND_STATUS,
    };
    for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        CHECK_TRUE(PANEL_CMD_IS_READ(reads[i]));
    }
}

TEST(protocol_command_macros) {
    CHECK_TRUE(PANEL_CMD_IS_READ(0x80));
    CHECK_TRUE(PANEL_CMD_IS_READ(0xC1));
    CHECK_TRUE(!PANEL_CMD_IS_READ(0x42));
    CHECK_TRUE(PANEL_CMD_IS_WRITE(0x42));
    CHECK_TRUE(!PANEL_CMD_IS_WRITE(0x85));
    CHECK_TRUE(PANEL_CMD_IS_ASYNC(0x42));
    CHECK_TRUE(!PANEL_CMD_IS_ASYNC(0x80));   // POLL_STATUS is read, not async
    CHECK_EQ_HEX(PANEL_CMD_DIR_MASK, 0x80);
    CHECK_EQ_HEX(PANEL_CMD_ASYNC_FLAG, 0x40);
}

TEST(protocol_special_constants) {
    CHECK_EQ_HEX(PANEL_ALIVE_MAGIC, 0xA5);
    CHECK_EQ_HEX(PANEL_ARG_EXTENDED, 0xFFFF);
    CHECK_EQ_HEX(PANEL_ARG_IGNORED, 0x0000);
    CHECK_EQ_INT(PANEL_PROTOCOL_MAX_PAYLOAD, 4096);
    // A -1 "parent directory" index, cast to the 16-bit argument field, must
    // read back as the extended sentinel — host_comm relies on this for the
    // SELECT_ENTRY "go up" encoding.
    CHECK_EQ_HEX((uint16_t)(int16_t)-1, PANEL_ARG_EXTENDED);
}

/* The initiator status wire format is shared verbatim with the main board's
 * panel_protocol_defs_initiator.h. These pin the layout so a change on either
 * side that is not mirrored fails here rather than on a bench. */
TEST(initiator_struct_sizes) {
    CHECK_EQ_INT(sizeof(initiator_target_info_t), 50);
    CHECK_EQ_INT(sizeof(initiator_status_response_t), 42);
}

TEST(initiator_status_offsets) {
    CHECK_EQ_INT(offsetof(initiator_status_response_t, phase), 0);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, current_target_id), 1);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, initiator_id), 2);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, targets_found), 3);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, targets_imaged), 4);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, drives_imaged_mask), 5);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, speed_kbps), 6);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, current_filename), 8);
    CHECK_EQ_INT(offsetof(initiator_status_response_t, targets), 42);
}

TEST(initiator_target_info_offsets) {
    CHECK_EQ_INT(offsetof(initiator_target_info_t, scsi_id), 0);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, status), 3);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, sectorcount), 4);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, sectors_done), 12);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, vendor), 20);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, product), 29);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, sense_key), 46);
    CHECK_EQ_INT(offsetof(initiator_target_info_t, skip_reason), 49);
}

/* 0x59 must stay an async write command: the main board answers it from the
 * main loop, where the initiator state is not being mutated underneath it.
 * Turning it into a read command would serve it from the ISR instead. */
TEST(initiator_status_is_an_async_write_command) {
    CHECK_EQ_HEX(PANEL_CMD_GET_INITIATOR_STATUS, 0x59);
    CHECK_EQ_INT(PANEL_CMD_IS_READ(PANEL_CMD_GET_INITIATOR_STATUS), 0);
    CHECK_TRUE(PANEL_CMD_IS_ASYNC(PANEL_CMD_GET_INITIATOR_STATUS));
}

/* The mode byte rides in the device list's reserved area, so an older main
 * board that zeroes it reads as target mode. */
TEST(initiator_mode_byte_is_backward_compatible) {
    CHECK_EQ_INT(offsetof(device_list_response_t, reserved), 2);
    CHECK_EQ_HEX(PANEL_MODE_TARGET, 0x00);
    device_list_response_t list = {0};
    CHECK_EQ_HEX(PANEL_DEVLIST_MODE(&list), PANEL_MODE_TARGET);
}

/* Listed but not loadable: the flag has to live in padding so the struct size
 * is unchanged and an older panel simply ignores it. */
TEST(entry_flags_fit_existing_padding) {
    CHECK_EQ_INT(sizeof(dir_entry_info_t), 68);
    CHECK_EQ_INT(offsetof(dir_entry_info_t, entry_type), 64);
    CHECK_EQ_INT(offsetof(dir_entry_info_t, flags), 65);
    CHECK_EQ_HEX(PANEL_ENTRY_FLAG_NOT_LOADABLE, 0x01);
}

void run_protocol_defs_suite(void) {
    printf("protocol_defs:\n");
    RUN(protocol_struct_sizes);
    RUN(protocol_header_is_packed);
    RUN(protocol_playback_status_offsets);
    RUN(protocol_loaded_image_status_offsets);
    RUN(protocol_device_summary_offsets);
    RUN(protocol_header_wire_layout);
    RUN(protocol_write_commands_have_clear_direction_bit);
    RUN(protocol_delete_rename_opcode_values);
    RUN(protocol_read_commands_have_set_direction_bit);
    RUN(protocol_command_macros);
    RUN(protocol_special_constants);
    RUN(initiator_struct_sizes);
    RUN(initiator_status_offsets);
    RUN(initiator_target_info_offsets);
    RUN(initiator_status_is_an_async_write_command);
    RUN(initiator_mode_byte_is_backward_compatible);
    RUN(entry_flags_fit_existing_padding);
}
