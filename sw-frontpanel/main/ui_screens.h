#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdint.h>
#include "esp_err.h"
#include "display_manager.h"
#include "menu_system.h"

#define FIRMWARE_VERSION "v0.1.0"

// Logo bitmap (converted from the existing picoide array)
extern const uint8_t picoide_logo[];

typedef enum {
    SCREEN_SPLASH,
    SCREEN_MAIN_MENU,
    SCREEN_DISC_LIST,
    SCREEN_SETTINGS,
    SCREEN_WIFI_MENU,
    SCREEN_WIFI_NETWORKS,
    SCREEN_INFO,
    SCREEN_FIRMWARE_UPDATE
} screen_type_t;

esp_err_t ui_show_splash_screen(display_manager_t *display, uint32_t duration_ms);
esp_err_t ui_draw_menu(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_disc_list(display_manager_t *display, menu_t *menu);
esp_err_t ui_draw_status_bar(display_manager_t *display, const char *status);
esp_err_t ui_draw_info_screen(display_manager_t *display, const char *title, const char *info);
esp_err_t ui_draw_firmware_update(display_manager_t *display, const char *status, uint8_t progress);

#endif