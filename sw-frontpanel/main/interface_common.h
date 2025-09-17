#ifndef INTERFACE_COMMON_H
#define INTERFACE_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "host_comm.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Common interface operations that both menu and web can use
typedef struct {
    host_comm_t *host_comm;
    wifi_manager_t *wifi_manager;
} interface_context_t;

// System information structure
typedef struct {
    const char *firmware_version;
    const char *hardware_name;
    const char *transport_name;
    bool host_connected;
    uint32_t free_memory;
    uint32_t uptime_seconds;
    wifi_manager_state_t wifi_state;
    char wifi_ip[16];
    char current_disc[64];
} system_info_t;

// Disc operations
esp_err_t interface_get_disc_list(interface_context_t *ctx, disc_info_t *discs, size_t max_discs, size_t *disc_count);
esp_err_t interface_select_disc(interface_context_t *ctx, uint32_t disc_index);
esp_err_t interface_eject_disc(interface_context_t *ctx);
esp_err_t interface_get_current_disc(interface_context_t *ctx, char *disc_name, size_t max_len);

// WiFi operations
esp_err_t interface_wifi_scan(interface_context_t *ctx, wifi_ap_info_t *networks, size_t max_networks, size_t *found_networks);
esp_err_t interface_wifi_connect(interface_context_t *ctx, const char *ssid, const char *password);
esp_err_t interface_wifi_disconnect(interface_context_t *ctx);
esp_err_t interface_wifi_get_status(interface_context_t *ctx, wifi_manager_state_t *state, char *ip_address, size_t ip_max_len);

// System operations
esp_err_t interface_get_system_info(interface_context_t *ctx, system_info_t *info);

// Utility functions
const char* interface_wifi_state_string(wifi_manager_state_t state);
const char* interface_auth_mode_string(wifi_auth_mode_t auth_mode);

#ifdef __cplusplus
}
#endif

#endif // INTERFACE_COMMON_H