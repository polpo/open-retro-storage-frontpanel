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
    SCREEN_INFO,
    SCREEN_FIRMWARE_UPDATE,
    SCREEN_FIRMWARE_STATUS,
    SCREEN_COUNT  // Sentinel - must be last
} screen_type_t;

esp_err_t ui_show_splash_screen(display_manager_t *display);
esp_err_t ui_update_splash_progress(display_manager_t *display, const char *step_text, uint8_t progress);
esp_err_t ui_draw_menu(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_disc_list(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_status_bar(display_manager_t *display, const char *status);
esp_err_t ui_draw_info_screen(display_manager_t *display, const char *title, const char *info);
esp_err_t ui_draw_firmware_update(display_manager_t *display, const char *status, uint8_t progress);
esp_err_t ui_draw_status_screen(display_manager_t *display, const char *disc_name,
                               const char *directory_path, uint32_t image_index,
                               uint32_t total_images, const playback_status_t *playback_status,
                               bool title_changed);
bool ui_animate_status_screen(display_manager_t *display);
bool ui_menu_needs_animation(menu_t *menu);
bool ui_animate_menu(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_firmware_status(display_manager_t *display,
                                  uint32_t panel_current_ver, uint32_t panel_avail_ver, bool panel_update_avail,
                                  uint32_t main_current_ver, uint32_t main_avail_ver, bool main_update_avail,
                                  uint8_t selection);

#endif
