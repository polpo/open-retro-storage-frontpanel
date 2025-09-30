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

    // Allocate text scroll state if not already allocated
    if (!menu->text_scroll_state) {
        menu->text_scroll_state = calloc(1, sizeof(text_scroll_state_t));
    }

    text_scroll_state_t *scroll_state = (text_scroll_state_t *)menu->text_scroll_state;
    static uint32_t last_selected_index = 0xFFFFFFFF;

    // Reset scroll state if selected item changed
    if (scroll_state && last_selected_index != menu->selected_index) {
        uint8_t highlight_width = (menu->item_count > menu->visible_items) ? 122 : 128;
        menu_item_t *selected_item = menu_get_selected_item(menu);
        if (selected_item) {
            init_text_scroll_state(scroll_state, display, selected_item->text, highlight_width - 4);
        }
        last_selected_index = menu->selected_index;
    }

    // Draw each visible menu item
    bool scrolling_active = false;
    for (uint32_t i = 0; i < visible_count; i++) {
        uint8_t y_pos = (i + 1) * 8;
        uint8_t highlight_width = (menu->item_count > menu->visible_items) ? 122 : 128;

        // Highlight selected item
        if (i == cursor_pos) {
            display_manager_draw_box(display, 0, y_pos - 8, highlight_width, 8);
            display_manager_set_draw_color(display, 0); // Inverted text

            // Draw with scrolling if needed
            if (scroll_state) {
                scrolling_active = draw_text_scrolling(display, 2, y_pos, visible_items[i].text, highlight_width - 4, scroll_state);
            } else {
                // Fallback to normal drawing
                display_manager_draw_text(display, 2, y_pos, visible_items[i].text);
            }

            display_manager_set_draw_color(display, 1);
        } else {
            // Draw non-selected items normally (no scrolling)
            display_manager_draw_text(display, 2, y_pos, visible_items[i].text);
        }
    }

    // Mark menu as needing redraw if text is actively scrolling
    if (scrolling_active && scroll_state && scroll_state->needs_scroll) {
        menu->needs_redraw = true;
    }
    // Note: We don't clear needs_redraw here - let it stay set if navigation occurred

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

    // Clear the redraw flag - it will be set again if scrolling continues or navigation occurs
    menu_clear_redraw_flag(menu);

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
                               const playback_status_t *playback_status, bool title_changed) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    // Static text scroll state for status screen title
    static text_scroll_state_t title_scroll_state = {0};
    static playback_status_t last_playback_status = {0};
    static bool first_draw = true;

    // Only do full redraw when status changes or first draw
    bool status_changed = title_changed || first_draw ||
                         memcmp(&last_playback_status, playback_status, sizeof(playback_status_t)) != 0;

    if (status_changed) {
        display_manager_clear(display);
        if (playback_status) {
            memcpy(&last_playback_status, playback_status, sizeof(playback_status_t));
        }
        first_draw = false;
    }

    // Draw title bar background - always draw full width black box
    display_manager_set_font(display, u8g2_font_6x10_tf);
    display_manager_draw_box(display, 0, 0, 128, 10);
    display_manager_set_draw_color(display, 0);

    // Reinitialize scroll state only when title actually changed
    const char *title = (disc_name && disc_name[0]) ? disc_name : "No Disc Loaded";
    if (title_changed) {
        init_text_scroll_state(&title_scroll_state, display, title, 124);
    }

    // Draw title with scrolling support
    draw_text_scrolling(display, 2, 8, title, 124, &title_scroll_state);

    display_manager_set_draw_color(display, 1);

    // Only redraw the rest if status changed
    if (!status_changed) {
        display_manager_request_update(display);
        return ESP_OK;
    }

    // If no disc inserted, just show title bar
    if (!playback_status || !playback_status->disc_inserted) {
        display_manager_request_update(display);
        return ESP_OK;
    }

    int y = 20;
    display_manager_set_font(display, u8g2_font_waffle_t_all);

    // Draw disc type
    uint16_t type_icon = 0xe0ab; // Disc icon
    /*
    case PANEL_DISC_TYPE_DATA:  type_icon = 0xe2b7; break; // File icon
    case PANEL_DISC_TYPE_AUDIO: type_icon = 0xe05c; break; // Music icon
    case PANEL_DISC_TYPE_MIXED: type_icon = 0xe0ab; break; // Disc icon
    */
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
    } else {
        uint16_t status_icon = 0xe2b7; // File icon
        display_manager_draw_glyph(display, 14, y, status_icon);
        display_manager_set_font(display, u8g2_font_6x10_tf);
        display_manager_draw_text(display, 28, y, "Data track");
    }

    display_manager_request_update(display);

    return ESP_OK;
}
