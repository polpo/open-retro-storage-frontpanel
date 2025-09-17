#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "host_comm.h"
#include "interface_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_SERVER_PORT 80
#define WEB_SERVER_MAX_URI_LEN 128
#define WEB_SERVER_MAX_HANDLERS 16

typedef struct {
    httpd_handle_t server;
    bool initialized;
    bool running;
    uint16_t port;
    interface_context_t *interface_ctx;
} web_server_t;

// Server lifecycle
esp_err_t web_server_init(web_server_t *server, uint16_t port);
esp_err_t web_server_start(web_server_t *server);
esp_err_t web_server_stop(web_server_t *server);
esp_err_t web_server_deinit(web_server_t *server);

// Configuration
esp_err_t web_server_set_interface_context(web_server_t *server, interface_context_t *interface_ctx);

// Status
bool web_server_is_running(web_server_t *server);
uint16_t web_server_get_port(web_server_t *server);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H