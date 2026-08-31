// SPDX-License-Identifier: GPL-2.0-only
//
//  Tier 2: command-framing tests for host_comm.c.
//
//  These drive the real host_comm.c through the real transport.c routing layer
//  into a recording mock transport, then assert exactly which command code,
//  argument, and payload bytes get put on the wire — and how responses are
//  parsed back. This is the "validate SPI commands" coverage: it pins the
//  protocol framing without any hardware.

#include "test_framework.h"
#include "host_comm.h"
#include "mock_transport.h"
#include "esp_rom_crc.h"
#include <string.h>

static host_comm_t comm;

static void setup(void) {
    mock_reset();
    memset(&comm, 0, sizeof(comm));
    transport_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    host_comm_init(&comm, &cfg);
}

// --- Directory browsing ------------------------------------------------------

TEST(get_entry_count_frames_command_and_parses_be32) {
    setup();
    // Async result is a 4-byte big-endian count.
    uint8_t result[4] = { 0x00, 0x00, 0x01, 0x02 }; // 0x00000102 = 258
    mock_set_poll_result(result, sizeof(result));

    uint32_t count = 0;
    CHECK_ESP_OK(host_comm_get_entry_count(&comm, 0, &count));
    CHECK_EQ_INT(count, 258);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_TRUE(x != NULL);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_DIR_ENTRY_COUNT);
    CHECK_EQ_HEX(x->argument, 0);
    CHECK_EQ_INT(x->write_len, 0);
}

// This command carries the browse device in the argument
TEST(get_entry_count_carries_browse_device_in_argument) {
    setup();
    uint8_t result[4] = { 0x00, 0x00, 0x00, 0x07 };
    mock_set_poll_result(result, sizeof(result));

    uint32_t count = 0;
    CHECK_ESP_OK(host_comm_get_entry_count(&comm, 1, &count));
    CHECK_EQ_INT(count, 7);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_TRUE(x != NULL);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_DIR_ENTRY_COUNT);
    CHECK_EQ_HEX(x->argument, 1);
    CHECK_EQ_INT(x->write_len, 0);
}

// This command carries the browse device in the payload
TEST(get_entry_info_sends_index_in_argument_and_device_in_payload) {
    setup();
    dir_entry_info_t entry;
    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, "GAME.ISO");
    entry.entry_type = PANEL_ENTRY_TYPE_FILE;
    mock_set_poll_result(&entry, sizeof(entry));

    dir_entry_info_t got;
    CHECK_ESP_OK(host_comm_get_entry_info(&comm, 1, 7, &got));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_ENTRY_INFO);
    CHECK_EQ_HEX(x->argument, 7);
    // The index owns the argument, so the browsed device rides in payload[0].
    CHECK_EQ_INT(x->write_len, 1);
    CHECK_EQ_HEX(x->write_data[0], 1);
    CHECK_EQ_STR(got.name, "GAME.ISO");
    CHECK_EQ_HEX(got.entry_type, PANEL_ENTRY_TYPE_FILE);
}

// Test that we reject an index that would get confused with PANEL_ARG_EXTENDED
TEST(get_entry_info_rejects_index_that_would_alias_the_sentinel) {
    setup();
    dir_entry_info_t got;
    CHECK_EQ_INT(host_comm_get_entry_info(&comm, 0, PANEL_ARG_EXTENDED, &got),
                 ESP_ERR_INVALID_ARG);
    CHECK_EQ_INT(mock_xfer_count(), 0);   // nothing put on the wire
}

TEST(select_entry_parent_encodes_minus_one_and_device) {
    setup();
    uint8_t result = 0;                       // 1-byte result code
    mock_set_poll_result(&result, 1);

    CHECK_ESP_OK(host_comm_select_entry(&comm, -1, 0));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_SELECT_ENTRY);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED); // (uint16_t)(int16_t)-1
    CHECK_EQ_INT(x->write_len, 1);
    CHECK_EQ_HEX(x->write_data[0], 0);             // device_index byte
}

TEST(select_entry_targets_device_index_in_payload) {
    setup();
    uint8_t result = 0;
    mock_set_poll_result(&result, 1);

    CHECK_ESP_OK(host_comm_select_entry(&comm, 3, 2));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_SELECT_ENTRY);
    CHECK_EQ_HEX(x->argument, 3);
    CHECK_EQ_INT(x->write_len, 1);
    CHECK_EQ_HEX(x->write_data[0], 2);
}

// --- Image navigation: command code + device in argument ---------------------

TEST(image_nav_commands_carry_device_in_argument) {
    setup();
    mock_set_poll_ready_empty();
    CHECK_ESP_OK(host_comm_select_prev_image(&comm, 1));
    CHECK_EQ_HEX(mock_last_xfer()->command, PANEL_CMD_SELECT_PREV_IMAGE);
    CHECK_EQ_HEX(mock_last_xfer()->argument, 1);

    setup();
    mock_set_poll_ready_empty();
    CHECK_ESP_OK(host_comm_select_next_image(&comm, 4));
    CHECK_EQ_HEX(mock_last_xfer()->command, PANEL_CMD_SELECT_NEXT_IMAGE);
    CHECK_EQ_HEX(mock_last_xfer()->argument, 4);

    setup();
    mock_set_poll_ready_empty();
    CHECK_ESP_OK(host_comm_eject_image(&comm, 5));
    CHECK_EQ_HEX(mock_last_xfer()->command, PANEL_CMD_EJECT_IMAGE);
    CHECK_EQ_HEX(mock_last_xfer()->argument, 5);
}

// --- Synchronous read commands (result arrives in phase 2) -------------------

TEST(get_device_status_reads_single_byte) {
    setup();
    uint8_t status_byte = PANEL_DEVICE_STATUS_LOADED;
    mock_set_poll_result(&status_byte, 1);

    uint8_t status = 0xFF;
    CHECK_ESP_OK(host_comm_get_device_status(&comm, 3, &status));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_DEVICE_STATUS);
    CHECK_EQ_HEX(x->argument, 3);
    CHECK_EQ_INT(x->read_len, 1);
    CHECK_EQ_HEX(status, PANEL_DEVICE_STATUS_LOADED);
}

TEST(get_playback_status_reads_full_struct) {
    setup();
    panel_playback_status_t pb;
    memset(&pb, 0, sizeof(pb));
    pb.disc_inserted = 1;
    pb.disc_type = PANEL_DISC_TYPE_AUDIO;
    pb.alive_magic = PANEL_ALIVE_MAGIC;
    strcpy(pb.disc_name, "Dark Side");
    mock_set_poll_result(&pb, sizeof(pb));

    panel_playback_status_t got;
    memset(&got, 0, sizeof(got));
    CHECK_ESP_OK(host_comm_get_playback_status(&comm, 0, &got));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_PLAYBACK_STATUS);
    CHECK_EQ_INT(x->read_len, (int)sizeof(panel_playback_status_t));
    CHECK_EQ_HEX(got.disc_type, PANEL_DISC_TYPE_AUDIO);
    CHECK_EQ_HEX(got.alive_magic, PANEL_ALIVE_MAGIC);
    CHECK_EQ_STR(got.disc_name, "Dark Side");
}

// --- Liveness probe ----------------------------------------------------------

TEST(probe_alive_ok_on_valid_magic) {
    setup();
    panel_playback_status_t pb;
    memset(&pb, 0, sizeof(pb));
    pb.alive_magic = PANEL_ALIVE_MAGIC;
    mock_set_poll_result(&pb, sizeof(pb));

    CHECK_ESP_OK(host_comm_probe_alive(&comm, 0));
    CHECK_EQ_HEX(mock_xfer(0)->command, PANEL_CMD_GET_PLAYBACK_STATUS);
}

TEST(probe_alive_rejects_garbage) {
    // Reads from an absent SPI host "succeed" with floating-bus garbage
    // (typically constant 0x00 or 0xFF); the missing magic must fail the probe.
    panel_playback_status_t pb;

    setup();
    memset(&pb, 0x00, sizeof(pb));
    mock_set_poll_result(&pb, sizeof(pb));
    CHECK_EQ_INT(host_comm_probe_alive(&comm, 0), ESP_ERR_INVALID_RESPONSE);

    setup();
    memset(&pb, 0xFF, sizeof(pb));
    mock_set_poll_result(&pb, sizeof(pb));
    CHECK_EQ_INT(host_comm_probe_alive(&comm, 0), ESP_ERR_INVALID_RESPONSE);
}

TEST(probe_alive_propagates_transport_error) {
    setup();
    mock_set_xfer_error(ESP_FAIL);
    CHECK_EQ_INT(host_comm_probe_alive(&comm, 0), ESP_FAIL);
}

// --- Transport factory --------------------------------------------------------

TEST(factory_selects_ops_by_type) {
    extern const transport_ops_t transport_spi_ops;
    extern const transport_ops_t transport_i2c_ops;
    CHECK_TRUE(transport_get_ops(TRANSPORT_TYPE_SPI) == &transport_spi_ops);
    CHECK_TRUE(transport_get_ops(TRANSPORT_TYPE_I2C) == &transport_i2c_ops);
    CHECK_TRUE(transport_get_ops((transport_type_t)99) == NULL);
}

// --- Firmware / file transfer framing ---------------------------------------

TEST(read_firmware_chunk_sends_le32_offset) {
    setup();
    uint8_t chunk[256];
    for (int i = 0; i < 256; i++) chunk[i] = (uint8_t)i;
    mock_set_poll_result(chunk, sizeof(chunk));

    uint8_t out[256];
    CHECK_ESP_OK(host_comm_read_firmware_chunk(&comm, 0x1000, out, sizeof(out)));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_START_FIRMWARE_READ);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    CHECK_EQ_INT(x->write_len, 4);
    uint8_t expect[4] = { 0x00, 0x10, 0x00, 0x00 }; // 0x1000 little-endian
    CHECK_EQ_MEM(x->write_data, expect, 4);
    CHECK_EQ_MEM(out, chunk, sizeof(chunk));
}

TEST(read_firmware_chunk_rejects_size_mismatch) {
    setup();
    uint8_t shortchunk[128] = {0};
    mock_set_poll_result(shortchunk, sizeof(shortchunk)); // 128 returned...

    uint8_t out[256];
    // ...but 256 requested -> size mismatch is rejected.
    CHECK_EQ_INT(host_comm_read_firmware_chunk(&comm, 0, out, sizeof(out)),
                 ESP_ERR_INVALID_SIZE);
}

TEST(write_file_chunk_frames_crc16_and_payload) {
    setup();
    mock_set_poll_ready_empty();
    const uint8_t data[] = { 'h', 'e', 'l', 'l', 'o' };
    uint16_t expect_crc = (uint16_t)~esp_rom_crc16_be((uint16_t)~0xffff, data, sizeof(data));

    CHECK_ESP_OK(host_comm_write_file_chunk(&comm, data, sizeof(data)));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_WRITE_FILE_CHUNK);
    CHECK_EQ_HEX(x->argument, expect_crc);     // chunk CRC carried in argument
    CHECK_EQ_INT(x->write_len, sizeof(data));
    CHECK_EQ_MEM(x->write_data, data, sizeof(data));
}

TEST(start_file_upload_builds_start_struct_plus_filename) {
    setup();
    mock_set_poll_ready_empty();
    const char *fname = "/picoide.ini";
    CHECK_ESP_OK(host_comm_start_file_upload(&comm, fname, 1234));

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_START_FILE_UPLOAD);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    // payload = panel_file_upload_start_t + filename + NUL
    size_t expect_len = sizeof(panel_file_upload_start_t) + strlen(fname) + 1;
    CHECK_EQ_INT(x->write_len, (int)expect_len);

    const panel_file_upload_start_t *hdr =
        (const panel_file_upload_start_t *)x->write_data;
    CHECK_EQ_INT(hdr->file_size, 1234);
    CHECK_EQ_INT(hdr->filename_len, (int)strlen(fname));
    CHECK_EQ_STR((const char *)(x->write_data + sizeof(panel_file_upload_start_t)), fname);
}

// --- File delete / rename ----------------------------------------------------

TEST(delete_file_frames_command_and_path) {
    setup();
    uint8_t rc = PANEL_DELETE_OK;
    mock_set_poll_result(&rc, 1);
    const char *path = "/dir/old.iso";

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_delete_file(&comm, path, &result));
    CHECK_EQ_INT(result, PANEL_DELETE_OK);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_DELETE_FILE);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    CHECK_EQ_INT(x->write_len, (int)strlen(path) + 1);   // path + NUL
    CHECK_EQ_STR((const char *)x->write_data, path);
}

TEST(delete_file_propagates_result_code) {
    setup();
    uint8_t rc = PANEL_DELETE_ERROR_IN_USE;
    mock_set_poll_result(&rc, 1);

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_delete_file(&comm, "/x.iso", &result));
    CHECK_EQ_INT(result, PANEL_DELETE_ERROR_IN_USE);
}

TEST(rename_file_frames_old_then_new_path) {
    setup();
    uint8_t rc = PANEL_RENAME_OK;
    mock_set_poll_result(&rc, 1);
    const char *oldp = "/a.iso";
    const char *newp = "/sub/b.iso";

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_rename_file(&comm, oldp, newp, &result));
    CHECK_EQ_INT(result, PANEL_RENAME_OK);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_RENAME_FILE);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    // payload = oldpath\0newpath\0
    CHECK_EQ_INT(x->write_len, (int)(strlen(oldp) + 1 + strlen(newp) + 1));
    CHECK_EQ_STR((const char *)x->write_data, oldp);
    CHECK_EQ_STR((const char *)(x->write_data + strlen(oldp) + 1), newp);
}

TEST(rename_file_propagates_result_code) {
    setup();
    uint8_t rc = PANEL_RENAME_ERROR_EXISTS;
    mock_set_poll_result(&rc, 1);

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_rename_file(&comm, "/a.iso", "/b.iso", &result));
    CHECK_EQ_INT(result, PANEL_RENAME_ERROR_EXISTS);
}

// --- File / directory creation -----------------------------------------------

TEST(touch_file_frames_command_and_path) {
    setup();
    uint8_t rc = PANEL_TOUCH_OK;
    mock_set_poll_result(&rc, 1);
    const char *path = "/NE4.hda";

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_touch_file(&comm, path, &result));
    CHECK_EQ_INT(result, PANEL_TOUCH_OK);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_TOUCH_FILE);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    CHECK_EQ_INT(x->write_len, (int)strlen(path) + 1);   // path + NUL
    CHECK_EQ_STR((const char *)x->write_data, path);
}

TEST(mkdir_frames_command_and_path) {
    setup();
    uint8_t rc = PANEL_MKDIR_OK;
    mock_set_poll_result(&rc, 1);
    const char *path = "/CD3";

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_mkdir(&comm, path, &result));
    CHECK_EQ_INT(result, PANEL_MKDIR_OK);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_EQ_HEX(x->command, PANEL_CMD_MKDIR);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_EXTENDED);
    CHECK_EQ_INT(x->write_len, (int)strlen(path) + 1);
    CHECK_EQ_STR((const char *)x->write_data, path);
}

TEST(mkdir_propagates_result_code) {
    setup();
    uint8_t rc = PANEL_MKDIR_ERROR_EXISTS;
    mock_set_poll_result(&rc, 1);

    uint8_t result = 0xFF;
    CHECK_ESP_OK(host_comm_mkdir(&comm, "/CD3", &result));
    CHECK_EQ_INT(result, PANEL_MKDIR_ERROR_EXISTS);
}

// --- Error / edge paths ------------------------------------------------------

TEST(poll_timeout_propagates) {
    setup();
    mock_set_poll_never_ready();
    uint32_t count = 0;
    CHECK_EQ_INT(host_comm_get_entry_count(&comm, 0, &count), ESP_ERR_TIMEOUT);
}

TEST(transport_error_propagates_and_stops) {
    setup();
    mock_set_xfer_error(ESP_ERR_INVALID_STATE);
    CHECK_EQ_INT(host_comm_eject_image(&comm, 0), ESP_ERR_INVALID_STATE);
    CHECK_EQ_INT(mock_xfer_count(), 1);   // failed issue -> no poll follow-up
}

TEST(oversize_response_is_rejected) {
    setup();
    // entry_count expects 4 bytes; main board reports 8 -> invalid response.
    uint8_t big[8] = {0};
    mock_set_poll_result(big, sizeof(big));
    uint32_t count = 0;
    CHECK_EQ_INT(host_comm_get_entry_count(&comm, 0, &count),
                 ESP_ERR_INVALID_RESPONSE);
}

TEST(poll_async_error_state_propagates) {
    setup();
    mock_set_poll_error();
    uint32_t count = 0;
    CHECK_EQ_INT(host_comm_get_entry_count(&comm, 0, &count), ESP_FAIL);
}

TEST(null_args_are_rejected) {
    setup();
    uint32_t count;
    CHECK_EQ_INT(host_comm_get_entry_count(NULL, 0, &count), ESP_ERR_INVALID_ARG);
    CHECK_EQ_INT(host_comm_get_entry_count(&comm, 0, NULL), ESP_ERR_INVALID_ARG);
}

TEST(silent_board_still_reaches_the_wire) {
    setup();
    // comm.link_up == false means "the main board stopped answering", not
    // "this handle is gone". main.c's reconnect path clears it and then keeps
    // calling here to find out when the board comes back, so the call must
    // still go out on the wire.
    comm.link_up = false;
    uint8_t status_byte = PANEL_DEVICE_STATUS_LOADED;
    mock_set_poll_result(&status_byte, 1);

    uint8_t status = 0xFF;
    CHECK_ESP_OK(host_comm_get_device_status(&comm, 0, &status));
    CHECK_TRUE(mock_xfer(0) != NULL);
}

TEST(torn_down_handle_is_rejected) {
    setup();
    host_comm_deinit(&comm);

    uint8_t status = 0;
    CHECK_EQ_INT(host_comm_get_device_status(&comm, 0, &status), ESP_ERR_INVALID_ARG);
    CHECK_TRUE(mock_xfer(0) == NULL);
}

// --- Device list -------------------------------------------------------------

TEST(get_device_list_frames_command) {
    setup();
    struct __attribute__((packed)) {
        device_list_response_t header;
        device_summary_t devices[2];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.device_count = 2;
    reply.header.max_devices = 8;
    reply.devices[0].device_index = 0;
    reply.devices[0].device_type = PANEL_DEV_CATEGORY_FIXED;
    reply.devices[1].device_index = 3;
    reply.devices[1].device_type = PANEL_DEV_CATEGORY_OPTICAL;
    mock_set_poll_result(&reply, sizeof(reply));

    uint8_t buf[sizeof(reply)];
    device_list_response_t *list = (device_list_response_t *)buf;
    CHECK_ESP_OK(host_comm_get_device_list(&comm, list, sizeof(buf)));
    CHECK_EQ_INT(list->device_count, 2);
    CHECK_EQ_INT(list->devices[1].device_index, 3);
    CHECK_EQ_INT(list->devices[1].device_type, PANEL_DEV_CATEGORY_OPTICAL);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_TRUE(x != NULL);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_DEVICE_LIST);
    CHECK_EQ_INT(x->write_len, 0);
}

// Callers walk devices[] up to device_count, so a reply claiming more devices
// than it carries must not be taken at its word.
TEST(get_device_list_clamps_count_to_what_arrived) {
    setup();
    struct __attribute__((packed)) {
        device_list_response_t header;
        device_summary_t devices[2];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.device_count = 200;   // lies: only two summaries follow
    reply.header.max_devices = 8;
    mock_set_poll_result(&reply, sizeof(reply));

    uint8_t buf[sizeof(reply)];
    device_list_response_t *list = (device_list_response_t *)buf;
    CHECK_ESP_OK(host_comm_get_device_list(&comm, list, sizeof(buf)));
    CHECK_EQ_INT(list->device_count, 2);
}

// A header with no summaries at all is what BlueSCSI sends in initiator mode.
TEST(get_device_list_accepts_an_empty_list) {
    setup();
    device_list_response_t reply;
    memset(&reply, 0, sizeof(reply));
    mock_set_poll_result(&reply, sizeof(reply));

    uint8_t buf[sizeof(device_list_response_t) + 4 * sizeof(device_summary_t)];
    device_list_response_t *list = (device_list_response_t *)buf;
    CHECK_ESP_OK(host_comm_get_device_list(&comm, list, sizeof(buf)));
    CHECK_EQ_INT(list->device_count, 0);
}

/* ---- initiator status (BlueSCSI only) ---- */

TEST(get_initiator_status_frames_command_and_copies_response) {
    setup();

    /* A 42-byte header plus two 50-byte targets, as the main board sends it. */
    uint8_t result[sizeof(initiator_status_response_t) + 2 * sizeof(initiator_target_info_t)];
    memset(result, 0, sizeof(result));
    initiator_status_response_t *src = (initiator_status_response_t *)result;
    src->phase = PANEL_INITIATOR_PHASE_IMAGING;
    src->current_target_id = 3;
    src->initiator_id = 7;
    src->targets_found = 2;
    src->speed_kbps = 1450;
    strcpy(src->current_filename, "HD03_imaged.hda");
    src->targets[0].scsi_id = 3;
    src->targets[0].sectors_done = 1000;
    src->targets[1].scsi_id = 4;
    src->targets[1].skip_reason = PANEL_INITIATOR_SKIP_NO_SPACE;
    mock_set_poll_result(result, sizeof(result));

    uint8_t buf[sizeof(initiator_status_response_t) + 8 * sizeof(initiator_target_info_t)];
    initiator_status_response_t *resp = (initiator_status_response_t *)buf;
    size_t out_size = 0;
    CHECK_ESP_OK(host_comm_get_initiator_status(&comm, resp, sizeof(buf), &out_size));

    CHECK_EQ_INT(out_size, sizeof(result));
    CHECK_EQ_HEX(resp->phase, PANEL_INITIATOR_PHASE_IMAGING);
    CHECK_EQ_INT(resp->current_target_id, 3);
    CHECK_EQ_INT(resp->targets_found, 2);
    CHECK_EQ_INT(resp->speed_kbps, 1450);
    CHECK_TRUE(strcmp(resp->current_filename, "HD03_imaged.hda") == 0);
    CHECK_EQ_INT(resp->targets[1].skip_reason, PANEL_INITIATOR_SKIP_NO_SPACE);

    const mock_xfer_t *x = mock_xfer(0);
    CHECK_TRUE(x != NULL);
    CHECK_EQ_HEX(x->command, PANEL_CMD_GET_INITIATOR_STATUS);
    CHECK_EQ_HEX(x->argument, PANEL_ARG_IGNORED);
    CHECK_EQ_INT(x->write_len, 0);
}

/* The main board defers panel commands for as long as it holds the SCSI bus,
 * which is up to a minute while a drive spins up. The caller has to be able to
 * tell that apart from a dead link, so the timeout must surface as such. */
TEST(get_initiator_status_times_out_while_the_bus_is_busy) {
    setup();
    mock_set_poll_never_ready();

    uint8_t buf[sizeof(initiator_status_response_t) + 8 * sizeof(initiator_target_info_t)];
    size_t out_size = 12345;
    esp_err_t ret = host_comm_get_initiator_status(&comm, (initiator_status_response_t *)buf,
                                                   sizeof(buf), &out_size);
    CHECK_EQ_INT(ret, ESP_ERR_TIMEOUT);
    CHECK_EQ_INT(out_size, 12345);   /* untouched on failure */
}

TEST(get_initiator_status_rejects_a_buffer_too_small_for_the_header) {
    setup();
    uint8_t buf[8];
    esp_err_t ret = host_comm_get_initiator_status(&comm, (initiator_status_response_t *)buf,
                                                   sizeof(buf), NULL);
    CHECK_EQ_INT(ret, ESP_ERR_INVALID_ARG);
}

void run_host_comm_suite(void) {
    printf("host_comm:\n");
    RUN(get_entry_count_frames_command_and_parses_be32);
    RUN(get_entry_count_carries_browse_device_in_argument);
    RUN(get_entry_info_sends_index_in_argument_and_device_in_payload);
    RUN(get_entry_info_rejects_index_that_would_alias_the_sentinel);
    RUN(select_entry_parent_encodes_minus_one_and_device);
    RUN(select_entry_targets_device_index_in_payload);
    RUN(image_nav_commands_carry_device_in_argument);
    RUN(get_device_status_reads_single_byte);
    RUN(get_playback_status_reads_full_struct);
    RUN(probe_alive_ok_on_valid_magic);
    RUN(probe_alive_rejects_garbage);
    RUN(probe_alive_propagates_transport_error);
    RUN(factory_selects_ops_by_type);
    RUN(read_firmware_chunk_sends_le32_offset);
    RUN(read_firmware_chunk_rejects_size_mismatch);
    RUN(write_file_chunk_frames_crc16_and_payload);
    RUN(start_file_upload_builds_start_struct_plus_filename);
    RUN(delete_file_frames_command_and_path);
    RUN(delete_file_propagates_result_code);
    RUN(rename_file_frames_old_then_new_path);
    RUN(rename_file_propagates_result_code);
    RUN(touch_file_frames_command_and_path);
    RUN(mkdir_frames_command_and_path);
    RUN(mkdir_propagates_result_code);
    RUN(poll_timeout_propagates);
    RUN(transport_error_propagates_and_stops);
    RUN(oversize_response_is_rejected);
    RUN(poll_async_error_state_propagates);
    RUN(null_args_are_rejected);
    RUN(silent_board_still_reaches_the_wire);
    RUN(torn_down_handle_is_rejected);
    RUN(get_device_list_frames_command);
    RUN(get_device_list_clamps_count_to_what_arrived);
    RUN(get_device_list_accepts_an_empty_list);
    RUN(get_initiator_status_frames_command_and_copies_response);
    RUN(get_initiator_status_times_out_while_the_bus_is_busy);
    RUN(get_initiator_status_rejects_a_buffer_too_small_for_the_header);
}
