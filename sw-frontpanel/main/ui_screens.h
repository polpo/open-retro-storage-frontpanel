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

#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdint.h>
#include "esp_err.h"
#include "display_manager.h"
#include "menu_system.h"
#include "host_comm.h"

typedef enum {
    SCREEN_SPLASH,
    SCREEN_STATUS,
    SCREEN_MAIN_MENU,
    SCREEN_DISC_LIST,
    SCREEN_SETTINGS,
    SCREEN_WIFI_MENU,
    SCREEN_DISPLAY_SETTINGS,
    SCREEN_IDLE_MODE,
    SCREEN_IDLE_TIMEOUT,
    SCREEN_BLANK_TIMEOUT,
    SCREEN_INFO,
    SCREEN_FIRMWARE_UPDATE,
    SCREEN_FIRMWARE_STATUS,
    SCREEN_COUNT  // Sentinel - must be last
} screen_type_t;

esp_err_t ui_show_splash_screen(display_manager_t *display);
esp_err_t ui_update_splash_progress(display_manager_t *display, const char *step_text, uint8_t progress);
esp_err_t ui_update_splash_version(display_manager_t *display, const char *version);
esp_err_t ui_draw_menu(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_disc_list(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_status_bar(display_manager_t *display, const char *status);
esp_err_t ui_draw_info_screen(display_manager_t *display, const char *title, const char *info);
esp_err_t ui_draw_firmware_update(display_manager_t *display, const char *status, uint8_t progress);
esp_err_t ui_draw_status_screen(display_manager_t *display, const char *disc_name,
                               const loaded_image_status_t *image_status,
                               const playback_status_t *playback_status,
                               bool title_changed);
bool ui_animate_status_screen(display_manager_t *display);
bool ui_menu_needs_animation(menu_t *menu);
bool ui_animate_menu(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_firmware_status(display_manager_t *display,
                                  uint32_t current_ver, uint32_t avail_ver,
                                  bool update_avail);

#endif
