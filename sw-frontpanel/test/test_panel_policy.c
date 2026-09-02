// SPDX-License-Identifier: GPL-2.0-only
//
//  Tests for panel_policy.c: which devices the panel treats as removable, and
//  which rows the main menu carries as a result.
//
//  Every case here is a regression that reached main once. The device-type
//  namespace collision (PANEL_DEVICE_TYPE_SCSI == PANEL_DEV_CATEGORY_OPTICAL)
//  silently removed "Eject Image" from BlueSCSI whenever the device list was
//  unavailable; the row-position mapping is what replaced a fixed menu layout
//  with a computed one.

#include <string.h>

#include "test_framework.h"
#include "panel_policy.h"

// A device list holding two devices, built in a buffer big enough for both.
typedef struct {
    device_list_response_t header;
    device_summary_t devices[4];
} test_device_list_t;

static device_list_response_t *make_list(test_device_list_t *buf,
                                         const uint16_t *indices,
                                         const uint8_t *types,
                                         uint8_t count) {
    memset(buf, 0, sizeof(*buf));
    buf->header.device_count = count;
    buf->header.max_devices = 4;
    for (uint8_t i = 0; i < count; i++) {
        buf->devices[i].device_index = indices[i];
        buf->devices[i].device_type = types[i];
    }
    return &buf->header;
}

static void test_removable_categories(void) {
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_REMOVABLE));
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_OPTICAL));
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_FLOPPY));
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_MO));
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_SEQUENTIAL));
    CHECK_TRUE(device_category_is_removable(PANEL_DEV_CATEGORY_ZIP));

    CHECK_TRUE(!device_category_is_removable(PANEL_DEV_CATEGORY_FIXED));
    CHECK_TRUE(!device_category_is_removable(0x06));  // network
    CHECK_TRUE(!device_category_is_removable(0x09));  // unknown to us
    CHECK_TRUE(!device_category_is_removable(0xFF));
}

// The two device_type namespaces collide numerically, which is why a
// PANEL_DEVICE_TYPE_* value must never be fed to device_category_is_removable().
// BlueSCSI reports PANEL_DEVICE_TYPE_SCSI for every device it has.
static void test_device_type_and_category_are_separate_namespaces(void) {
    CHECK_EQ_INT(PANEL_DEVICE_TYPE_ATAPI, 0x00);
    CHECK_EQ_INT(PANEL_DEVICE_TYPE_IDE,   0x01);
    CHECK_EQ_INT(PANEL_DEVICE_TYPE_SCSI,  0x02);

    CHECK_EQ_INT(PANEL_DEV_CATEGORY_FIXED,   0x00);
    CHECK_EQ_INT(PANEL_DEV_CATEGORY_OPTICAL, 0x02);

    // Same byte value, opposite meaning: a SCSI hard disk would read as optical.
    CHECK_EQ_INT(PANEL_DEVICE_TYPE_SCSI, PANEL_DEV_CATEGORY_OPTICAL);
}

// No device list yet: the caller's fallback decides, so a BlueSCSI whose device
// list failed keeps its Eject row instead of losing it on every device.
static void test_no_device_list_uses_the_fallback(void) {
    CHECK_TRUE(device_list_is_removable(NULL, 0, true));
    CHECK_TRUE(!device_list_is_removable(NULL, 0, false));
    CHECK_TRUE(device_list_is_removable(NULL, 3, true));
}

static void test_active_device_looked_up_by_index(void) {
    test_device_list_t buf;
    const uint16_t indices[] = {0, 3};
    const uint8_t types[] = {PANEL_DEV_CATEGORY_FIXED, PANEL_DEV_CATEGORY_OPTICAL};
    device_list_response_t *list = make_list(&buf, indices, types, 2);

    CHECK_TRUE(!device_list_is_removable(list, 0, true));   // fixed disk at ID 0
    CHECK_TRUE(device_list_is_removable(list, 3, false));   // CD-ROM at ID 3
}

// Initiator mode: the list is valid and empty. Nothing to eject, whatever the
// fallback says.
static void test_empty_list_is_not_removable(void) {
    test_device_list_t buf;
    device_list_response_t *list = make_list(&buf, NULL, NULL, 0);

    CHECK_TRUE(!device_list_is_removable(list, 0, true));
    CHECK_TRUE(!device_list_is_removable(list, 0, false));
}

// An index the list does not carry is not removable, even with a true fallback:
// the list is authoritative once we have one.
static void test_unlisted_index_is_not_removable(void) {
    test_device_list_t buf;
    const uint16_t indices[] = {0, 3};
    const uint8_t types[] = {PANEL_DEV_CATEGORY_OPTICAL, PANEL_DEV_CATEGORY_OPTICAL};
    device_list_response_t *list = make_list(&buf, indices, types, 2);

    CHECK_TRUE(!device_list_is_removable(list, 5, true));
}

static void test_menu_layout_single_fixed_device(void) {
    main_menu_layout_t l;
    main_menu_layout(false, false, &l);

    CHECK_EQ_INT(l.count, 3);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_IMAGE], 0);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SETTINGS], 1);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SYSTEM_INFO], 2);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_EJECT_IMAGE], -1);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_DEVICE], -1);
}

static void test_menu_layout_single_removable_device(void) {
    main_menu_layout_t l;
    main_menu_layout(true, false, &l);

    CHECK_EQ_INT(l.count, 4);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_IMAGE], 0);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_EJECT_IMAGE], 1);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SETTINGS], 2);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SYSTEM_INFO], 3);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_DEVICE], -1);
}

static void test_menu_layout_multi_device_fixed(void) {
    main_menu_layout_t l;
    main_menu_layout(false, true, &l);

    CHECK_EQ_INT(l.count, 4);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_IMAGE], 0);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_DEVICE], 1);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SETTINGS], 2);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SYSTEM_INFO], 3);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_EJECT_IMAGE], -1);
}

static void test_menu_layout_multi_device_removable(void) {
    main_menu_layout_t l;
    main_menu_layout(true, true, &l);

    CHECK_EQ_INT(l.count, MAIN_MENU_MAX_ITEMS);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_IMAGE], 0);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_EJECT_IMAGE], 1);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SELECT_DEVICE], 2);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SETTINGS], 3);
    CHECK_EQ_INT(l.position[MAIN_MENU_ROW_SYSTEM_INFO], 4);
}

// An absent row must never match a real menu position. Position 0 exists in
// every layout, so -1 is the only safe "absent" marker.
static void test_absent_rows_never_alias_a_position(void) {
    const bool flags[] = {false, true};
    for (int e = 0; e < 2; e++) {
        for (int m = 0; m < 2; m++) {
            main_menu_layout_t l;
            main_menu_layout(flags[e], flags[m], &l);
            for (int row = 0; row < MAIN_MENU_ROW_COUNT; row++) {
                if (l.position[row] < 0) {
                    CHECK_EQ_INT(l.position[row], -1);
                } else {
                    CHECK_TRUE(l.position[row] < (int8_t)l.count);
                    CHECK_EQ_INT(l.rows[l.position[row]], row);
                }
            }
        }
    }
}

// Every layout renders text for every row it claims to have.
static void test_every_present_row_has_text(void) {
    main_menu_layout_t l;
    main_menu_layout(true, true, &l);
    for (uint32_t i = 0; i < l.count; i++) {
        CHECK_TRUE(strlen(main_menu_row_text(l.rows[i])) > 0);
    }
}

void run_panel_policy_suite(void) {
    RUN(test_removable_categories);
    RUN(test_device_type_and_category_are_separate_namespaces);
    RUN(test_no_device_list_uses_the_fallback);
    RUN(test_active_device_looked_up_by_index);
    RUN(test_empty_list_is_not_removable);
    RUN(test_unlisted_index_is_not_removable);
    RUN(test_menu_layout_single_fixed_device);
    RUN(test_menu_layout_single_removable_device);
    RUN(test_menu_layout_multi_device_fixed);
    RUN(test_menu_layout_multi_device_removable);
    RUN(test_absent_rows_never_alias_a_position);
    RUN(test_every_present_row_has_text);
}
