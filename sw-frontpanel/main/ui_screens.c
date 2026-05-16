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

#include "ui_screens.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_screens";

// Horizontal text scrolling state
typedef struct {
    int16_t offset;              // Current scroll offset in pixels
    uint32_t last_update_ms;     // Last time scroll was updated
    uint32_t pause_start_ms;     // When current pause started
    bool is_paused;              // Currently paused?
    bool pause_at_end;           // Paused at end or beginning?
    bool needs_scroll;           // Does text actually need scrolling?
} text_scroll_state_t;

#define TEXT_SCROLL_SPEED_PX_PER_SEC 25
#define TEXT_SCROLL_INITIAL_PAUSE_MS 2000
#define TEXT_SCROLL_END_PAUSE_MS 1000
#define TEXT_SCROLL_UPDATE_INTERVAL_MS 50  // 20 FPS for smooth scrolling

// Helper function to update text scroll state and return current offset
static int16_t update_text_scroll_offset(text_scroll_state_t *state, uint16_t text_width, uint16_t display_width) {
    if (!state || !state->needs_scroll) {
        return 0;
    }

    uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Handle initial pause at beginning
    if (state->is_paused && !state->pause_at_end) {
        if (current_ms - state->pause_start_ms >= TEXT_SCROLL_INITIAL_PAUSE_MS) {
            state->is_paused = false;
            state->last_update_ms = current_ms;
        }
        return 0;
    }

    // Handle pause at end
    if (state->is_paused && state->pause_at_end) {
        if (current_ms - state->pause_start_ms >= TEXT_SCROLL_END_PAUSE_MS) {
            // Reset to beginning
            state->offset = 0;
            state->is_paused = true;
            state->pause_at_end = false;
            state->pause_start_ms = current_ms;
        }
        return state->offset;
    }

    // Update scroll offset
    uint32_t elapsed_ms = current_ms - state->last_update_ms;
    if (elapsed_ms >= TEXT_SCROLL_UPDATE_INTERVAL_MS) {
        int16_t pixels_to_move = (TEXT_SCROLL_SPEED_PX_PER_SEC * elapsed_ms) / 1000;
        state->offset += pixels_to_move;
        state->last_update_ms = current_ms;

        // Check if we've scrolled past the end
        int16_t max_offset = text_width - display_width + 10; // +10 for some padding
        if (state->offset >= max_offset) {
            state->offset = max_offset;
            state->is_paused = true;
            state->pause_at_end = true;
            state->pause_start_ms = current_ms;
        }
    }

    return state->offset;
}

// Helper function to initialize/reset text scroll state
static void init_text_scroll_state(text_scroll_state_t *state, display_manager_t *display,
                                   const char *text, uint16_t available_width) {
    if (!state || !display || !text) {
        return;
    }

    // Measure text width
    uint16_t text_width = u8g2_GetStrWidth(&display->u8g2, text);

    // Determine if scrolling is needed
    state->needs_scroll = (text_width > available_width);
    state->offset = 0;
    state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    state->pause_start_ms = state->last_update_ms;
    state->is_paused = true;
    state->pause_at_end = false;
}

// Helper function to draw scrolling text with clipping
// Returns true if scrolling is active and needs continuous updates
static bool draw_text_scrolling(display_manager_t *display, uint8_t x, uint8_t y,
                                const char *text, uint16_t width, text_scroll_state_t *state) {
    if (!display || !text || !state) {
        return false;
    }

    if (!state->needs_scroll) {
        // No scrolling needed, draw normally
        display_manager_draw_text(display, x, y, text);
        return false;
    }

    // Get current scroll offset
    uint16_t text_width = u8g2_GetStrWidth(&display->u8g2, text);
    int16_t offset = update_text_scroll_offset(state, text_width, width);

    // Calculate text position (use signed int to handle negative positions)
    int16_t text_x = (int16_t)x - offset;

    // Set clipping region
    u8g2_SetClipWindow(&display->u8g2, x, 0, x + width, 64);

    // Draw text with offset (u8g2 handles negative coordinates)
    u8g2_DrawStr(&display->u8g2, text_x, y, text);

    // Reset clipping
    u8g2_SetMaxClipWindow(&display->u8g2);

    // Return true to indicate scrolling is active
    return true;
}

// Logo bitmap from original code
static const uint8_t picoide_logo[] = {
  0xF0, 0xFF, 0x0F, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFC, 
  0xFF, 0x80, 0xFF, 0xFF, 0xF0, 0xFF, 0x1F, 0x7E, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x80, 0x3F, 0xFE, 0xFF, 0xC3, 0xFF, 0x7F, 0xF0, 0x83, 0x1F, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x1F, 0x7E, 0xF8, 0xC3, 0x0F, 0x00, 
  0xF8, 0x81, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x1F, 0x7E, 
  0xF0, 0xC7, 0x0F, 0x00, 0xF8, 0xC1, 0x1F, 0x3F, 0xFC, 0xFF, 0x87, 0xFF, 
  0xFF, 0x81, 0x1F, 0x7F, 0xF0, 0xE7, 0x0F, 0x00, 0xF8, 0xC1, 0x8F, 0x3F, 
  0xFE, 0xFF, 0xC3, 0xFF, 0xFF, 0xC3, 0x1F, 0x3F, 0xF0, 0xE3, 0x07, 0x00, 
  0xFC, 0xC1, 0x8F, 0x1F, 0x7F, 0x00, 0xE0, 0x07, 0xF0, 0xC7, 0x0F, 0x3F, 
  0xF0, 0xF3, 0x0F, 0x00, 0xFC, 0xF1, 0x87, 0x1F, 0x3F, 0x00, 0xF0, 0x07, 
  0xF0, 0xC7, 0x0F, 0x3F, 0xF0, 0xFB, 0xFF, 0x00, 0xFC, 0xFE, 0x83, 0x1F, 
  0x3F, 0x00, 0xF0, 0x03, 0xF0, 0xC7, 0x8F, 0x3F, 0xF8, 0xF3, 0x7F, 0x00, 
  0xFC, 0x00, 0xC0, 0x9F, 0x1F, 0x00, 0xF0, 0x03, 0xF0, 0xE3, 0x87, 0x1F, 
  0xF8, 0xF1, 0x03, 0x00, 0xFE, 0x00, 0xC0, 0x8F, 0x1F, 0x00, 0xF8, 0x03, 
  0xF0, 0xE3, 0x87, 0x1F, 0xF8, 0xF1, 0x03, 0x00, 0x7E, 0x00, 0xC0, 0x8F, 
  0x1F, 0x00, 0xF8, 0x03, 0xF0, 0xE3, 0x87, 0x1F, 0xFC, 0xF1, 0x03, 0x00, 
  0x7E, 0x00, 0xC0, 0x8F, 0x3F, 0x00, 0xF0, 0x07, 0xF8, 0xF1, 0xC7, 0x1F, 
  0xFE, 0xF8, 0x03, 0x00, 0x7F, 0x00, 0xE0, 0x07, 0xFF, 0xFF, 0xF0, 0xFF, 
  0xFF, 0xF0, 0xC3, 0xFF, 0x7F, 0xF8, 0xFF, 0x0F, 0x3F, 0x00, 0xE0, 0x07, 
  0xFE, 0xFF, 0xC0, 0xFF, 0x7F, 0xF0, 0xC3, 0xFF, 0x3F, 0xF8, 0xFF, 0x0F
};



esp_err_t ui_show_splash_screen(display_manager_t *display) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_clear(display);
    display_manager_draw_bitmap(display, 0, 0, 128, 15, picoide_logo);

    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);

    const esp_app_desc_t* app_desc = esp_app_get_description();
    char version_text[64];
    snprintf(version_text, sizeof(version_text), "FW v%s", app_desc->version);
    display_manager_draw_text(display, 30, 32, version_text);

    display_manager_draw_frame(display, 20, 40, 88, 8);

    display_manager_request_update(display);

    ESP_LOGI(TAG, "Splash screen initialized");
    return ESP_OK;
}

esp_err_t ui_update_splash_progress(display_manager_t *display, const char *step_text, uint8_t progress) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_set_draw_color(display, 0);
    display_manager_draw_box(display, 20, 50, 128, 12);

    display_manager_set_draw_color(display, 1);
    display_manager_set_font(display, u8g2_font_6x10_tf);
    display_manager_draw_text(display, 22, 60, step_text ? step_text : "");

    uint8_t fill_width = (progress * 84) / 100;
    display_manager_draw_box(display, 22, 42, fill_width, 4);

    display_manager_request_update(display);

    return ESP_OK;
}

esp_err_t ui_update_splash_versions(display_manager_t *display, const char *panel_version, const char *main_version) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear the version text area
    display_manager_set_draw_color(display, 0);
    display_manager_draw_box(display, 0, 18, 128, 20);
    display_manager_set_draw_color(display, 1);

    display_manager_set_font(display, u8g2_font_6x10_tf);

    char version_text[32];
    snprintf(version_text, sizeof(version_text), "Panel: v%s", panel_version);
    display_manager_draw_text(display, 22, 26, version_text);

    snprintf(version_text, sizeof(version_text), "Main:  %s%s", main_version ? "v" : "", main_version ? main_version : "...");
    display_manager_draw_text(display, 22, 36, version_text);

    display_manager_request_update(display);

    return ESP_OK;
}

esp_err_t ui_draw_menu(display_manager_t *display, menu_t *menu) {
    if (!display || !menu) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear display
    display_manager_clear(display);

    // Draw header if title provided
    uint8_t y_offset = 0;
    if (menu->title) {
        display_manager_set_font(display, u8g2_font_6x10_tf);
        display_manager_draw_box(display, 0, 0, 128, 10);
        display_manager_set_draw_color(display, 0);
        display_manager_draw_text(display, 2, 8, menu->title);
        display_manager_set_draw_color(display, 1);
        y_offset = 10;
    }

    // Get visible menu items
    menu_item_t *visible_items;
    uint32_t visible_count;
    uint32_t cursor_pos;

    esp_err_t ret = menu_get_visible_items(menu, &visible_items, &visible_count, &cursor_pos);
    if (ret != ESP_OK) {
        return ret;
    }

    // Set font for menu items
    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);

    // Allocate text scroll state if not already allocated
    if (!menu->text_scroll_state) {
        menu->text_scroll_state = calloc(1, sizeof(text_scroll_state_t));
    }

    text_scroll_state_t *scroll_state = (text_scroll_state_t *)menu->text_scroll_state;
    uint8_t highlight_width = (menu->item_count > menu->visible_items) ? 122 : 128;

    // Initialize scroll state for selected item (animation function will use this)
    if (scroll_state && cursor_pos < visible_count) {
        init_text_scroll_state(scroll_state, display, visible_items[cursor_pos].text, highlight_width - 4);
    }

    // Draw each visible menu item
    for (uint32_t i = 0; i < visible_count; i++) {
        uint8_t y_pos = y_offset + (i + 1) * 8;

        // Highlight selected item
        if (i == cursor_pos) {
            display_manager_draw_box(display, 0, y_pos - 8, highlight_width, 8);
            display_manager_set_draw_color(display, 0); // Inverted text
            display_manager_draw_text(display, 2, y_pos, visible_items[i].text);
            display_manager_set_draw_color(display, 1);
        } else {
            // Draw non-selected items normally
            display_manager_draw_text(display, 2, y_pos, visible_items[i].text);
        }
    }

    // Draw scroll bar if there are more items than visible
    if (menu->item_count > menu->visible_items) {
        // Calculate scroll bar dimensions
        uint8_t scrollbar_x = 125;
        uint8_t scrollbar_y = y_offset;
        uint8_t scrollbar_width = 3;
        uint8_t scrollbar_height = 64 - y_offset;

        // Draw scroll bar background (thin frame)
        display_manager_draw_frame(display, scrollbar_x, scrollbar_y, scrollbar_width, scrollbar_height);

        // Calculate thumb position and size
        uint8_t thumb_height = (menu->visible_items * (scrollbar_height - 2)) / menu->item_count;
        if (thumb_height < 4) thumb_height = 4; // Minimum thumb size

        uint8_t max_thumb_pos = scrollbar_height - 2 - thumb_height;
        uint8_t thumb_y = scrollbar_y + 1 + (menu->window_start * max_thumb_pos) / (menu->item_count - menu->visible_items);

        // Draw scroll thumb (filled box)
        display_manager_draw_box(display, scrollbar_x + 1, thumb_y, scrollbar_width - 2, thumb_height);
    }

    // Request display update
    display_manager_request_update(display);

    // Clear redraw flag - full draw is complete
    // Animation will be triggered separately via ui_menu_needs_animation()
    menu_clear_redraw_flag(menu);

    return ESP_OK;
}

// Check if menu has active scrolling animation
bool ui_menu_needs_animation(menu_t *menu) {
    if (!menu) {
        return false;
    }
    text_scroll_state_t *scroll_state = (text_scroll_state_t *)menu->text_scroll_state;
    return scroll_state && scroll_state->needs_scroll;
}

// Lightweight menu animation - only updates the selected item's scrolling text
// Returns true if animation is active and display was updated
bool ui_animate_menu(display_manager_t *display, menu_t *menu) {
    if (!display || !menu) {
        return false;
    }

    text_scroll_state_t *scroll_state = (text_scroll_state_t *)menu->text_scroll_state;
    if (!scroll_state || !scroll_state->needs_scroll) {
        return false;
    }

    // Get cursor position and visible items info
    menu_item_t *visible_items;
    uint32_t visible_count;
    uint32_t cursor_pos;

    esp_err_t ret = menu_get_visible_items(menu, &visible_items, &visible_count, &cursor_pos);
    if (ret != ESP_OK || cursor_pos >= visible_count) {
        return false;
    }

    // Calculate y position
    uint8_t y_offset = menu->title ? 10 : 0;
    uint8_t y_pos = y_offset + (cursor_pos + 1) * 8;
    uint8_t highlight_width = (menu->item_count > menu->visible_items) ? 122 : 128;

    // Set font and colors
    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);

    // Clear and redraw just the selected item row
    display_manager_set_draw_color(display, 1);
    display_manager_draw_box(display, 0, y_pos - 8, highlight_width, 8);
    display_manager_set_draw_color(display, 0);

    // Draw scrolling text
    draw_text_scrolling(display, 2, y_pos, visible_items[cursor_pos].text, highlight_width - 4, scroll_state);

    display_manager_set_draw_color(display, 1);
    display_manager_request_update(display);

    return true;
}

esp_err_t ui_draw_disc_list(display_manager_t *display, menu_t *menu) {
    // Use the regular menu drawing - menu->title should be set appropriately
    return ui_draw_menu(display, menu);
}

esp_err_t ui_draw_status_bar(display_manager_t *display, const char *status) {
    if (!display || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Draw status bar at bottom of screen
    display_manager_set_font(display, u8g2_font_6x10_tf);
    display_manager_draw_box(display, 0, 56, 128, 8);
    display_manager_set_draw_color(display, 0);
    display_manager_draw_text(display, 2, 63, status);
    display_manager_set_draw_color(display, 1);
    
    display_manager_request_update(display);
    
    return ESP_OK;
}

esp_err_t ui_draw_info_screen(display_manager_t *display, const char *title, const char *info) {
    if (!display || !title || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    display_manager_clear(display);
    
    // Draw title
    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);
    display_manager_draw_box(display, 0, 0, 128, 10);
    display_manager_set_draw_color(display, 0);
    display_manager_draw_text(display, 2, 8, title);
    display_manager_set_draw_color(display, 1);
    
    // Draw info text
    display_manager_set_font(display, u8g2_font_6x10_tf);
    
    // Simple word wrap - this could be improved
    int y = 20;
    int line_height = 10;
    char line[32];
    const char *p = info;
    
    while (*p && y < 64) {
        int len = 0;
        while (*p && *p != '\n' && len < 20) {
            line[len++] = *p++;
        }
        line[len] = '\0';
        
        display_manager_draw_text(display, 2, y, line);
        y += line_height;
        
        if (*p == '\n') p++;
    }

    display_manager_request_update(display);

    return ESP_OK;
}

esp_err_t ui_draw_firmware_update(display_manager_t *display, const char *status, uint8_t progress) {
    if (!display || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_clear(display);

    // Draw title
    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);
    display_manager_draw_text(display, 10, 12, "Firmware Update");

    // Draw status text
    display_manager_set_font(display, u8g2_font_6x10_tr);
    display_manager_draw_text(display, 5, 28, status);

    // Draw progress bar frame
    display_manager_draw_frame(display, 10, 40, 108, 12);

    // Draw progress bar fill
    if (progress > 0) {
        uint8_t fill_width = (progress * 104) / 100;
        display_manager_set_draw_color(display, 1);
        display_manager_draw_box(display, 12, 42, fill_width, 8);
    }

    // Draw progress percentage
    char progress_text[16];
    snprintf(progress_text, sizeof(progress_text), "%d%%", progress);
    display_manager_draw_text(display, 54, 62, progress_text);

    display_manager_request_update(display);

    // FW updates count as activity — keep the display awake so dim/screensaver
    // don't engage over the progress UI.
    display_manager_wake(display);

    return ESP_OK;
}

// Helper to format version number to string
static void format_version_string(char *buf, size_t buf_size, uint32_t version) {
    uint8_t major = (version >> 24) & 0xFF;
    uint8_t minor = (version >> 16) & 0xFF;
    uint8_t patch = (version >> 8) & 0xFF;
    uint8_t pre = version & 0xFF;
    if (pre == 0xFF) {
        snprintf(buf, buf_size, "%d.%d.%d", major, minor, patch);
    } else {
        snprintf(buf, buf_size, "%d.%d.%d-pre%d", major, minor, patch, pre);
    }
}

esp_err_t ui_draw_firmware_status(display_manager_t *display,
                                  uint32_t current_ver, uint32_t avail_ver,
                                  bool update_avail) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_clear(display);

    // Title bar
    display_manager_set_font(display, u8g2_font_6x10_tf);
    display_manager_draw_box(display, 0, 0, 128, 12);
    display_manager_set_draw_color(display, 0);
    display_manager_draw_text(display, 2, 10, "Firmware Update");
    display_manager_set_draw_color(display, 1);

    char version_str[20];

    // System version (main board version is user-facing)
    display_manager_draw_text(display, 2, 26, "Version:");
    format_version_string(version_str, sizeof(version_str), current_ver);
    display_manager_draw_text(display, 56, 26, version_str);

    // Available version
    if (update_avail) {
        display_manager_draw_text(display, 2, 40, "Available:");
        format_version_string(version_str, sizeof(version_str), avail_ver);
        display_manager_draw_text(display, 68, 40, version_str);
    } else {
        display_manager_draw_text(display, 2, 40, "Up to date");
    }

    // Instructions at bottom
    if (update_avail) {
        display_manager_draw_text(display, 0, 62, "[>] Update  [<] Back");
    } else {
        display_manager_draw_text(display, 0, 62, "[<] Back");
    }

    display_manager_request_update(display);

    return ESP_OK;
}

// Helper to extract directory name from path (returns pointer into path)
// Examples: "/cdrom" -> "cdrom", "/foo/bar" -> "bar", "/" -> "/", "cdrom" -> "cdrom"
static const char* get_directory_name(const char *path) {
    if (!path || !path[0]) {
        return "/";
    }

    size_t len = strlen(path);

    // Skip trailing slashes
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    // Find last slash before the end
    const char *end = path + len;
    const char *last_slash = NULL;
    for (const char *p = path; p < end; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }

    // If no slash, or only a leading slash with content after, return after the slash
    if (!last_slash) {
        return path;  // No slash at all, e.g., "cdrom"
    }
    if (last_slash == path && len > 1) {
        return path + 1;  // Leading slash only, e.g., "/cdrom" -> "cdrom"
    }
    if (last_slash == path && len == 1) {
        return "/";  // Root directory
    }
    return last_slash + 1;  // e.g., "/foo/bar" -> "bar"
}

// Module-level state for status screen animation
static text_scroll_state_t status_title_scroll_state = {0};
static char status_title_text[64] = "";

esp_err_t ui_draw_status_screen(display_manager_t *display, const char *disc_name,
                               const loaded_image_status_t *image_status,
                               const playback_status_t *playback_status,
                               bool title_changed) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_clear(display);

    bool has_disc = (disc_name && disc_name[0] && playback_status && playback_status->disc_inserted);

    // === TOP BAR: Directory name + position indicator (inverted) ===
    display_manager_set_font(display, u8g2_font_6x10_tf);
    display_manager_draw_box(display, 0, 0, 128, 10);
    display_manager_set_draw_color(display, 0);

    if (has_disc && image_status && image_status->directory_path[0]) {
        // Get just the directory name
        const char *dir_name = get_directory_name(image_status->directory_path);

        // Calculate position indicator width if we have image count
        char pos_indicator[16] = "";
        int pos_indicator_width = 0;
        if (image_status->total_images > 0) {
            snprintf(pos_indicator, sizeof(pos_indicator), "[%lu/%lu]",
                    (unsigned long)(image_status->image_index + 1),
                    (unsigned long)image_status->total_images);
            pos_indicator_width = u8g2_GetStrWidth(&display->u8g2, pos_indicator);
        }

        // Draw position indicator on the right
        if (pos_indicator_width > 0) {
            display_manager_draw_text(display, 126 - pos_indicator_width, 8, pos_indicator);
        }

        // Draw directory name on the left (truncate if needed)
        int available_width = 124 - pos_indicator_width - 4;  // Leave margin
        int dir_name_width = u8g2_GetStrWidth(&display->u8g2, dir_name);

        if (dir_name_width <= available_width) {
            display_manager_draw_text(display, 2, 8, dir_name);
        } else {
            // Truncate with ellipsis
            char truncated[32];
            int len = strlen(dir_name);
            int max_chars = (available_width / 6) - 2;  // Approx 6px per char, minus "..."
            if (max_chars > 0 && max_chars < len) {
                snprintf(truncated, sizeof(truncated), "%.*s...", max_chars, dir_name);
                display_manager_draw_text(display, 2, 8, truncated);
            } else {
                display_manager_draw_text(display, 2, 8, dir_name);
            }
        }
    }

    display_manager_set_draw_color(display, 1);

    // === CENTER: Large image name with scrolling (regular text) ===
    const char *title = has_disc ? disc_name : "No Disc Loaded";

    display_manager_set_font(display, u8g2_font_helvR12_tf);

    // Only reinitialize scroll state when title actually changed
    if (title_changed) {
        strncpy(status_title_text, title, sizeof(status_title_text) - 1);
        status_title_text[sizeof(status_title_text) - 1] = '\0';
        init_text_scroll_state(&status_title_scroll_state, display, status_title_text, 124);
    }

    // Draw image name
    draw_text_scrolling(display, 2, 32, status_title_text, 124, &status_title_scroll_state);

    // If no disc inserted, we're done
    if (!has_disc) {
        display_manager_request_update(display);
        return ESP_OK;
    }

    // === BOTTOM: Playback status ===
    int y = 52;
    display_manager_set_font(display, u8g2_font_waffle_t_all);

    // Draw device type icon
    uint16_t type_icon;
    if (playback_status->disc_type == PANEL_DISC_TYPE_HDD) {
        type_icon = 0xe2cd;  // HDD/storage icon
    } else {
        type_icon = 0xe0ab;  // Disc icon
    }
    display_manager_draw_glyph(display, 2, y, type_icon);

    // For audio/mixed discs, show playback status
    if (playback_status->disc_type == PANEL_DISC_TYPE_AUDIO ||
        playback_status->disc_type == PANEL_DISC_TYPE_MIXED) {

        uint16_t status_icon;

        switch (playback_status->audio_status) {
            case PANEL_AUDIO_STATUS_PLAYING:
                status_icon = 0xe058;  // Play icon
                break;
            case PANEL_AUDIO_STATUS_PAUSED:
                status_icon = 0xe059;  // Pause icon
                break;
            case PANEL_AUDIO_STATUS_PLAYING_COMPLETED:
            case PANEL_AUDIO_STATUS_NONE:
            default:
                status_icon = 0xe057;  // Stop icon
                break;
        }

        display_manager_draw_glyph(display, 14, y, status_icon);

        display_manager_draw_glyph(display, 40, y, 0xe016);

        // Show track and time if playing or paused
        if (playback_status->is_playing ||
            playback_status->audio_status == PANEL_AUDIO_STATUS_PAUSED) {
            char track_line[32];
            display_manager_set_font(display, u8g2_font_6x10_tf);
            snprintf(track_line, sizeof(track_line), "%02d", playback_status->current_track);
            display_manager_draw_text(display, 28, y, track_line);

            char time_line[32];
            snprintf(time_line, sizeof(time_line), "%02d:%02d",
                    playback_status->track_position_m,
                    playback_status->track_position_s);
            display_manager_draw_text(display, 52, y, time_line);
        }
    } else if (playback_status->disc_type == PANEL_DISC_TYPE_HDD) {
        // IDE hard disk mode - show CHS and size
        display_manager_set_font(display, u8g2_font_6x10_tf);

        if (image_status && image_status->cylinders > 0) {
            // Calculate size in MB
            uint64_t total_sectors = (uint64_t)image_status->cylinders *
                                     image_status->heads * image_status->sectors;
            uint32_t size_mb = (uint32_t)((total_sectors * 512) / (1024 * 1024));

            char chs_line[24];
            snprintf(chs_line, sizeof(chs_line), "%u/%u/%u %luMB",
                     image_status->cylinders,
                     image_status->heads,
                     image_status->sectors,
                     (unsigned long)size_mb);
            display_manager_draw_text(display, 16, y, chs_line);
        } else {
            display_manager_draw_text(display, 16, y, "IDE Hard Disk");
        }
    } else {
        // Data CD-ROM
        uint16_t status_icon = 0xe2b7; // File icon
        display_manager_draw_glyph(display, 14, y, status_icon);
        display_manager_set_font(display, u8g2_font_6x10_tf);
        display_manager_draw_text(display, 28, y, "Data track");
    }

    display_manager_request_update(display);

    return ESP_OK;
}

// Lightweight animation function - only updates scrolling text area
// Returns true if animation is active and display was updated
bool ui_animate_status_screen(display_manager_t *display) {
    if (!display || !status_title_scroll_state.needs_scroll) {
        return false;
    }

    // Only redraw the scrolling text area
    display_manager_set_font(display, u8g2_font_helvR12_tf);

    // Clear just the center text area
    display_manager_set_draw_color(display, 0);
    display_manager_draw_box(display, 0, 12, 128, 24);
    display_manager_set_draw_color(display, 1);

    // Draw scrolling text
    draw_text_scrolling(display, 2, 32, status_title_text, 124, &status_title_scroll_state);

    display_manager_request_update(display);
    return true;
}
