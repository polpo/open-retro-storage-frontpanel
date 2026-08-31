// SPDX-License-Identifier: GPL-2.0-only
//
//  Copyright (C) 2025  Ian Scott
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License (as published by the
//  Free Software Foundation) version 2, dated June 1991.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, see <https://www.gnu.org/licenses/>.

// Decisions about what the panel offers for the device it is showing: whether
// that device's media can be ejected, and which rows the main menu carries as a
// result. Kept free of ESP-IDF so the host tests in test/ can exercise them.

#ifndef PANEL_POLICY_H
#define PANEL_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "panel_protocol_defs.h"

// Whether a PANEL_DEV_CATEGORY_* value describes removable media. This is the
// set BlueSCSI's physical eject button handles, so the panel, the web UI and
// the button agree. A category we do not recognise is not removable.
bool device_category_is_removable(uint8_t category);

// Removability of the device at active_index according to list. A NULL list
// means none has been fetched and the answer is fallback; a list that does not
// carry active_index answers false.
//
// Do not substitute loaded_image_status_t.device_type for this: that field is a
// PANEL_DEVICE_TYPE_* (a transport kind), not a PANEL_DEV_CATEGORY_*, and the
// two namespaces collide numerically. BlueSCSI reports PANEL_DEVICE_TYPE_SCSI
// for every device whatever its media.
bool device_list_is_removable(const device_list_response_t *list,
                              uint16_t active_index, bool fallback);

// Main menu rows, in the order they are shown when present.
typedef enum {
    MAIN_MENU_ROW_SELECT_IMAGE = 0,
    MAIN_MENU_ROW_EJECT_IMAGE,
    MAIN_MENU_ROW_SELECT_DEVICE,
    MAIN_MENU_ROW_SETTINGS,
    MAIN_MENU_ROW_SYSTEM_INFO,
    MAIN_MENU_ROW_COUNT
} main_menu_row_t;

#define MAIN_MENU_MAX_ITEMS MAIN_MENU_ROW_COUNT

typedef struct {
    uint32_t count;                        // rows actually present
    main_menu_row_t rows[MAIN_MENU_MAX_ITEMS];  // rows[position] -> row identity
    int8_t position[MAIN_MENU_ROW_COUNT];  // position[row] -> row position, -1 if absent
} main_menu_layout_t;

// Lay out the main menu: "Eject Image" only when the active device has
// removable media, "Select Device" only when more than one device exists.
void main_menu_layout(bool ejectable, bool multi, main_menu_layout_t *out);

// Display text for a row.
const char *main_menu_row_text(main_menu_row_t row);

#endif // PANEL_POLICY_H
