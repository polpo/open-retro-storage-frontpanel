#include "ui_screens.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_screens";

// Logo bitmap from original code
const uint8_t picoide_logo[] = {
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

esp_err_t ui_draw_menu(display_manager_t *display, menu_t *menu) {
    if (!display || !menu) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Clear display
    display_manager_clear(display);
    
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
    
    // Draw each visible menu item
    for (uint32_t i = 0; i < visible_count; i++) {
        uint8_t y_pos = (i + 1) * 8;
        
        // Highlight selected item
        if (i == cursor_pos) {
            uint8_t highlight_width = (menu->item_count > menu->visible_items) ? 122 : 128;
            display_manager_draw_box(display, 0, y_pos - 8, highlight_width, 8);
            display_manager_set_draw_color(display, 0); // Inverted text
        }
        
        // Draw menu item text
        display_manager_draw_text(display, 2, y_pos, visible_items[i].text);
        
        // Reset color if it was inverted
        if (i == cursor_pos) {
            display_manager_set_draw_color(display, 1);
        }
    }
    
    // Draw scroll bar if there are more items than visible
    if (menu->item_count > menu->visible_items) {
        // Calculate scroll bar dimensions
        uint8_t scrollbar_x = 124;
        uint8_t scrollbar_y = 0;
        uint8_t scrollbar_width = 3;
        uint8_t scrollbar_height = 64;
        
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
    
    return ESP_OK;
}

esp_err_t ui_draw_disc_list(display_manager_t *display, menu_t *menu) {
    // For now, just use the regular menu drawing
    // Could be customized later with icons or different formatting
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

    return ESP_OK;
}

esp_err_t ui_draw_status_screen(display_manager_t *display, const char *disc_name,
                               const playback_status_t *playback_status) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    display_manager_clear(display);

    // Draw title bar with disc name or "No Disc Loaded"
    const char *title = (disc_name && disc_name[0]) ? disc_name : "No Disc Loaded";
    display_manager_set_font(display, u8g2_font_amstrad_cpc_extended_8f);
    display_manager_draw_box(display, 0, 0, 128, 10);
    display_manager_set_draw_color(display, 0);
    display_manager_draw_text(display, 2, 8, title);
    display_manager_set_draw_color(display, 1);

    // If no disc inserted, just show title bar
    if (!playback_status || !playback_status->disc_inserted) {
        display_manager_request_update(display);
        return ESP_OK;
    }

    int y = 20;
    display_manager_set_font(display, u8g2_font_6x10_tf);

    // Draw disc type
    const char *type_str = "Unknown";
    switch (playback_status->disc_type) {
        case PANEL_DISC_TYPE_DATA:  type_str = "Data"; break;
        case PANEL_DISC_TYPE_AUDIO: type_str = "Audio"; break;
        case PANEL_DISC_TYPE_MIXED: type_str = "Mixed"; break;
    }
    char type_line[32];
    snprintf(type_line, sizeof(type_line), "Type: %s", type_str);
    display_manager_draw_text(display, 2, y, type_line);
    y += 12;

    // For audio/mixed discs, show playback status
    if (playback_status->disc_type == PANEL_DISC_TYPE_AUDIO ||
        playback_status->disc_type == PANEL_DISC_TYPE_MIXED) {

        const char *status_icon = " ";
        const char *status_str = "Stopped";

        switch (playback_status->audio_status) {
            case PANEL_AUDIO_STATUS_PLAYING:
                status_icon = "\x10";  // Play icon
                status_str = "Playing";
                break;
            case PANEL_AUDIO_STATUS_PAUSED:
                status_icon = "\x11";  // Pause icon
                status_str = "Paused";
                break;
            case PANEL_AUDIO_STATUS_PLAYING_COMPLETED:
                status_str = "Completed";
                break;
            case PANEL_AUDIO_STATUS_NONE:
                status_str = "Stopped";
                break;
        }

        char status_line[32];
        snprintf(status_line, sizeof(status_line), "%s %s", status_icon, status_str);
        display_manager_draw_text(display, 2, y, status_line);
        y += 12;

        // Show track and time if playing or paused
        if (playback_status->is_playing ||
            playback_status->audio_status == PANEL_AUDIO_STATUS_PAUSED) {
            char track_line[32];
            snprintf(track_line, sizeof(track_line), "Track: %02d", playback_status->current_track);
            display_manager_draw_text(display, 2, y, track_line);
            y += 10;

            char time_line[32];
            snprintf(time_line, sizeof(time_line), "Time: %02d:%02d",
                    playback_status->track_position_m,
                    playback_status->track_position_s);
            display_manager_draw_text(display, 2, y, time_line);
        }
    }

    display_manager_request_update(display);

    return ESP_OK;
}
