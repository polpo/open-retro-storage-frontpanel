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

#include "panel_policy.h"

// Mirrors EJECTABLE_TYPES in www/app.js and device_type_is_ejectable() in the
// BlueSCSI main board's panel_protocol.cpp: keep the three in sync.
bool device_category_is_removable(uint8_t category) {
    switch (category) {
        case PANEL_DEV_CATEGORY_REMOVABLE:
        case PANEL_DEV_CATEGORY_OPTICAL:
        case PANEL_DEV_CATEGORY_FLOPPY:
        case PANEL_DEV_CATEGORY_MO:
        case PANEL_DEV_CATEGORY_SEQUENTIAL:
        case PANEL_DEV_CATEGORY_ZIP:
            return true;
        default:
            return false;
    }
}

bool device_list_is_removable(const device_list_response_t *list,
                              uint16_t active_index, bool fallback) {
    if (!list) {
        return fallback;
    }
    for (uint8_t i = 0; i < list->device_count; i++) {
        if (list->devices[i].device_index == active_index) {
            return device_category_is_removable(list->devices[i].device_type);
        }
    }
    return false;
}

static void layout_append(main_menu_layout_t *out, main_menu_row_t row) {
    out->rows[out->count] = row;
    out->position[row] = (int8_t)out->count;
    out->count++;
}

void main_menu_layout(bool ejectable, bool multi, main_menu_layout_t *out) {
    out->count = 0;
    for (int i = 0; i < MAIN_MENU_ROW_COUNT; i++) {
        out->position[i] = -1;
    }

    layout_append(out, MAIN_MENU_ROW_SELECT_IMAGE);
    if (ejectable) {
        layout_append(out, MAIN_MENU_ROW_EJECT_IMAGE);
    }
    if (multi) {
        layout_append(out, MAIN_MENU_ROW_SELECT_DEVICE);
    }
    layout_append(out, MAIN_MENU_ROW_SETTINGS);
    layout_append(out, MAIN_MENU_ROW_SYSTEM_INFO);
}

const char *main_menu_row_text(main_menu_row_t row) {
    switch (row) {
        case MAIN_MENU_ROW_SELECT_IMAGE:  return "Select Image";
        case MAIN_MENU_ROW_EJECT_IMAGE:   return "Eject Image";
        case MAIN_MENU_ROW_SELECT_DEVICE: return "Select Device";
        case MAIN_MENU_ROW_SETTINGS:      return "Settings";
        case MAIN_MENU_ROW_SYSTEM_INFO:   return "System Info";
        default:                          return "";
    }
}
