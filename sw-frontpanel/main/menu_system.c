#include "menu_system.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

// static const char *TAG = "menu_system";

esp_err_t menu_init(menu_t *menu, uint32_t visible_items) {
    if (!menu) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(menu, 0, sizeof(menu_t));
    menu->visible_items = (visible_items > 0 && visible_items <= MENU_VISIBLE_ITEMS) 
                         ? visible_items : MENU_VISIBLE_ITEMS;
    menu->needs_redraw = true;
    
    return ESP_OK;
}

esp_err_t menu_set_items(menu_t *menu, menu_item_t *items, uint32_t count) {
    if (!menu || !items || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    menu->items = items;
    menu->item_count = count;
    menu->selected_index = 0;
    menu->cursor_position = 0;
    menu->window_start = 0;
    menu->needs_redraw = true;

    return ESP_OK;
}

esp_err_t menu_add_item(menu_t *menu, const char *text, menu_action_t action,
                        void (*callback)(void *), void *context) {
    if (!menu || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    if (menu->item_count >= MENU_MAX_ITEMS) {
        return ESP_ERR_NO_MEM;
    }

    if (!menu->items) {
        menu->items = calloc(MENU_MAX_ITEMS, sizeof(menu_item_t));
        if (!menu->items) {
            return ESP_ERR_NO_MEM;
        }
    }

    menu_item_t *item = &menu->items[menu->item_count];
    strncpy(item->text, text, MENU_ITEM_MAX_LENGTH - 1);
    item->text[MENU_ITEM_MAX_LENGTH - 1] = '\0';
    item->action = action;
    item->callback = callback;
    item->callback_context = context;
    item->selectable = true;
    
    menu->item_count++;
    menu->needs_redraw = true;

    return ESP_OK;
}

esp_err_t menu_clear_items(menu_t *menu) {
    if (!menu) {
        return ESP_ERR_INVALID_ARG;
    }

    if (menu->items) {
        free(menu->items);
        menu->items = NULL;
    }

    if (menu->text_scroll_state) {
        free(menu->text_scroll_state);
        menu->text_scroll_state = NULL;
    }

    menu->item_count = 0;
    menu->selected_index = 0;
    menu->cursor_position = 0;
    menu->window_start = 0;
    menu->needs_redraw = true;

    return ESP_OK;
}

esp_err_t menu_navigate_up(menu_t *menu) {
    if (!menu || menu->item_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (menu->selected_index == 0) {
        return ESP_OK;  // Already at top
    }

    menu->selected_index--;
    
    if (menu->cursor_position > 0) {
        menu->cursor_position--;
    } else {
        menu->window_start--;
    }

    menu->needs_redraw = true;

    if (menu->on_change) {
        menu->on_change(menu->selected_index, &menu->items[menu->selected_index]);
    }

    return ESP_OK;
}

esp_err_t menu_navigate_down(menu_t *menu) {
    if (!menu || menu->item_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (menu->selected_index >= menu->item_count - 1) {
        return ESP_OK;  // Already at bottom
    }

    menu->selected_index++;
    
    if (menu->cursor_position < menu->visible_items - 1) {
        menu->cursor_position++;
    } else {
        menu->window_start++;
    }

    menu->needs_redraw = true;

    if (menu->on_change) {
        menu->on_change(menu->selected_index, &menu->items[menu->selected_index]);
    }

    return ESP_OK;
}

esp_err_t menu_select_current(menu_t *menu) {
    if (!menu || menu->item_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    menu_item_t *item = &menu->items[menu->selected_index];
    
    if (!item->selectable) {
        return ESP_OK;
    }

    if (item->callback) {
        item->callback(item->callback_context);
    }

    if (menu->on_select) {
        menu->on_select(menu->selected_index, item);
    }

    menu->needs_redraw = true;

    return ESP_OK;
}

uint32_t menu_get_selected_index(menu_t *menu) {
    if (!menu) {
        return 0;
    }
    return menu->selected_index;
}

menu_item_t* menu_get_selected_item(menu_t *menu) {
    if (!menu || menu->item_count == 0) {
        return NULL;
    }
    return &menu->items[menu->selected_index];
}

bool menu_needs_redraw(menu_t *menu) {
    if (!menu) {
        return false;
    }
    return menu->needs_redraw;
}

void menu_clear_redraw_flag(menu_t *menu) {
    if (menu) {
        menu->needs_redraw = false;
    }
}

esp_err_t menu_get_visible_items(menu_t *menu, menu_item_t **items, 
                                 uint32_t *count, uint32_t *cursor_pos) {
    if (!menu || !items || !count || !cursor_pos) {
        return ESP_ERR_INVALID_ARG;
    }

    if (menu->item_count == 0) {
        *items = NULL;
        *count = 0;
        *cursor_pos = 0;
        return ESP_OK;
    }

    *items = &menu->items[menu->window_start];
    *count = (menu->item_count - menu->window_start) < menu->visible_items 
            ? (menu->item_count - menu->window_start) 
            : menu->visible_items;
    *cursor_pos = menu->cursor_position;

    return ESP_OK;
}