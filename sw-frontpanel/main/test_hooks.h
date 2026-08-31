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

#ifndef TEST_HOOKS_H
#define TEST_HOOKS_H

#ifdef CONFIG_PANEL_TEST_HOOKS

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_UI_LABEL_MAX 64

// Snapshot of what the panel is currently showing. current_screen and
// active_menu are file-static in main.c and stay that way; this is filled by an
// accessor there so the web layer never reaches into main.c's internals.
typedef struct {
    int  screen;                          // screen_type_t as int
    const char *screen_name;              // "SCREEN_DISC_LIST", static storage
    bool has_menu;                        // false when the screen has no menu
    uint32_t cursor;                      // selected_index within the menu
    uint32_t count;                       // item_count
    int32_t entry_index;                  // selected row's host entry index, -1 if none
    char selected[PANEL_UI_LABEL_MAX];    // selected row's label, "" if none
    int  display_state;                   // display_state_t as int
    const char *display_state_name;       // static storage
} panel_ui_state_t;

// Fill *out with the current UI state. Safe to call from the httpd task: it
// only reads, and a torn read costs at worst one stale field in a test.
void panel_get_ui_state(panel_ui_state_t *out);

#define PANEL_FB_MAX_BYTES 1024   // 64x128 device, 8x16 tiles

// Copy out the last frame sent to the panel. Returns bytes written, 0 on
// failure. This is a shadow of the frame that reached the glass, not the live
// u8g2 buffer, which ui_draw_menu paints across many calls and would tear.
//
// The bytes are u8g2's native column-major tile format for the panel's own
// orientation, NOT a linear bitmap: de-tiling and un-rotating are left to the
// host so the firmware stays trivial and the rendering can change without a
// reflash. *tile_w / *tile_h are in 8-pixel tiles, *rotation is the U8G2_R*
// index the display was set up with.
size_t panel_get_framebuffer(uint8_t *out, size_t cap, int *tile_w, int *tile_h, int *rotation);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_PANEL_TEST_HOOKS
#endif // TEST_HOOKS_H
