#ifndef MENU_SYSTEM_H
#define MENU_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MENU_MAX_ITEMS 256
#define MENU_VISIBLE_ITEMS 8
#define MENU_ITEM_MAX_LENGTH 64

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_SELECT,
    MENU_ACTION_BACK,
    MENU_ACTION_CUSTOM
} menu_action_t;

typedef struct menu_item {
    char text[MENU_ITEM_MAX_LENGTH];
    menu_action_t action;
    void (*callback)(void *context);
    void *callback_context;
    struct menu_item *submenu;
    bool selectable;
} menu_item_t;

typedef struct {
    const char *title;       // Menu title displayed in header
    menu_item_t *items;
    uint32_t item_count;
    uint32_t selected_index;
    uint32_t cursor_position;
    uint32_t window_start;
    uint32_t visible_items;
    bool needs_redraw;
    void (*on_select)(uint32_t index, menu_item_t *item);
    void (*on_change)(uint32_t index, menu_item_t *item);
    void *text_scroll_state; // Opaque pointer to text_scroll_state_t for selected item
} menu_t;

esp_err_t menu_init(menu_t *menu, uint32_t visible_items);
esp_err_t menu_set_items(menu_t *menu, menu_item_t *items, uint32_t count);
esp_err_t menu_add_item(menu_t *menu, const char *text, menu_action_t action, 
                       void (*callback)(void *), void *context);
esp_err_t menu_clear_items(menu_t *menu);
esp_err_t menu_navigate_up(menu_t *menu);
esp_err_t menu_navigate_down(menu_t *menu);
esp_err_t menu_select_current(menu_t *menu);
uint32_t menu_get_selected_index(menu_t *menu);
menu_item_t* menu_get_selected_item(menu_t *menu);
bool menu_needs_redraw(menu_t *menu);
void menu_clear_redraw_flag(menu_t *menu);
esp_err_t menu_get_visible_items(menu_t *menu, menu_item_t **items, 
                                 uint32_t *count, uint32_t *cursor_pos);

#endif