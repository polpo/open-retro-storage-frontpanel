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

#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include "interface_common.h"
#include "json_stream.h"
#include "ota_manager.h"

static const char *TAG = "web_server";

// Forward declarations
static esp_err_t static_file_handler(httpd_req_t *req);
static esp_err_t api_status_handler(httpd_req_t *req);
static esp_err_t api_images_handler(httpd_req_t *req);
static esp_err_t api_select_entry_handler(httpd_req_t *req);
static esp_err_t api_eject_image_handler(httpd_req_t *req);
static esp_err_t api_prev_image_handler(httpd_req_t *req);
static esp_err_t api_next_image_handler(httpd_req_t *req);
static esp_err_t api_wifi_scan_handler(httpd_req_t *req);
static esp_err_t api_wifi_connect_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_handler(httpd_req_t *req);
static esp_err_t api_firmware_check_handler(httpd_req_t *req);
static esp_err_t api_firmware_update_handler(httpd_req_t *req);
static esp_err_t api_firmware_status_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_check_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_update_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_status_handler(httpd_req_t *req);
static esp_err_t api_upload_handler(httpd_req_t *req);

// Global server instance for handlers
static web_server_t *g_server = NULL;

// Global OTA manager instance
static ota_manager_t g_ota_manager;

// Embedded file content (gzipped, symbols created by CMakeLists.txt)
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t sha256_js_gz_start[] asm("_binary_sha256_js_gz_start");
extern const uint8_t sha256_js_gz_end[] asm("_binary_sha256_js_gz_end");

// Static file descriptor for common handler
typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *content_type;
} static_file_t;

static const static_file_t file_index_html = {
    index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8"
};
static const static_file_t file_app_js = {
    app_js_gz_start, app_js_gz_end, "application/javascript; charset=utf-8"
};
static const static_file_t file_sha256_js = {
    sha256_js_gz_start, sha256_js_gz_end, "application/javascript; charset=utf-8"
};

// URI handlers
static const httpd_uri_t uri_handlers[] = {
    { .uri = "/", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_index_html },
    { .uri = "/app.js", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_app_js },
    { .uri = "/sha256.js", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_sha256_js },
    { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL },
    { .uri = "/api/images", .method = HTTP_GET, .handler = api_images_handler, .user_ctx = NULL },
    { .uri = "/api/select_entry", .method = HTTP_POST, .handler = api_select_entry_handler, .user_ctx = NULL },
    { .uri = "/api/eject_image", .method = HTTP_POST, .handler = api_eject_image_handler, .user_ctx = NULL },
    { .uri = "/api/prev_image", .method = HTTP_POST, .handler = api_prev_image_handler, .user_ctx = NULL },
    { .uri = "/api/next_image", .method = HTTP_POST, .handler = api_next_image_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = api_wifi_status_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/check", .method = HTTP_GET, .handler = api_firmware_check_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/update", .method = HTTP_POST, .handler = api_firmware_update_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/status", .method = HTTP_GET, .handler = api_firmware_status_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/check", .method = HTTP_GET, .handler = api_mainboard_firmware_check_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/update", .method = HTTP_POST, .handler = api_mainboard_firmware_update_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/status", .method = HTTP_GET, .handler = api_mainboard_firmware_status_handler, .user_ctx = NULL },
    { .uri = "/api/upload", .method = HTTP_POST, .handler = api_upload_handler, .user_ctx = NULL }
};

static esp_err_t static_file_handler(httpd_req_t *req) {
    const static_file_t *file = (const static_file_t *)req->user_ctx;
    httpd_resp_set_type(req, file->content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)file->start, file->end - file->start);
}

static esp_err_t api_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx) {
        system_info_t sys_info;
        esp_err_t info_ret = interface_get_system_info(g_server->interface_ctx, &sys_info);
        if (info_ret == ESP_OK) {
            ret = json.write("firmware", sys_info.firmware_version);
            if (ret != ESP_OK) return ret;
            ret = json.write("hardware", sys_info.hardware_name);
            if (ret != ESP_OK) return ret;
            ret = json.write("transport", sys_info.transport_name);
            if (ret != ESP_OK) return ret;
            ret = json.write("host_connected", sys_info.host_connected ? 1 : 0);
            if (ret != ESP_OK) return ret;
            ret = json.write("free_memory", sys_info.free_memory);
            if (ret != ESP_OK) return ret;
            ret = json.write("uptime", sys_info.uptime_seconds);
            if (ret != ESP_OK) return ret;
        } else {
            ret = json.write("firmware", "v0.1.0");
            if (ret != ESP_OK) return ret;
            ret = json.write("hardware", "ESP32-C3");
            if (ret != ESP_OK) return ret;
            ret = json.write("transport", "None");
            if (ret != ESP_OK) return ret;
            ret = json.write("host_connected", 0);
            if (ret != ESP_OK) return ret;
            ret = json.write("free_memory", (uint32_t)esp_get_free_heap_size());
            if (ret != ESP_OK) return ret;
            ret = json.write("uptime", (uint32_t)(esp_log_timestamp() / 1000));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("firmware", "v0.1.0");
        if (ret != ESP_OK) return ret;
        ret = json.write("hardware", "ESP32-C3");
        if (ret != ESP_OK) return ret;
        ret = json.write("transport", "None");
        if (ret != ESP_OK) return ret;
        ret = json.write("host_connected", 0);
        if (ret != ESP_OK) return ret;
        ret = json.write("free_memory", (uint32_t)esp_get_free_heap_size());
        if (ret != ESP_OK) return ret;
        ret = json.write("uptime", (uint32_t)(esp_log_timestamp() / 1000));
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

// HTTP response writer for ArduinoJson streaming
class HttpResponseWriter {
public:
    HttpResponseWriter(httpd_req_t* req) : req_(req) {}

    size_t write(uint8_t b) {
        char c = (char)b;
        esp_err_t ret = httpd_resp_send_chunk(req_, &c, 1);
        return (ret == ESP_OK) ? 1 : 0;
    }

    size_t write(const uint8_t* s, size_t n) {
        esp_err_t ret = httpd_resp_send_chunk(req_, (const char*)s, n);
        return (ret == ESP_OK) ? n : 0;
    }

private:
    httpd_req_t* req_;
};

static esp_err_t api_images_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx) {
        host_comm_t *host_comm = g_server->interface_ctx->host_comm;

        // Get current path
        char current_path[256] = "/";
        if (host_comm && host_comm->initialized) {
            host_comm_get_current_path(host_comm, current_path, sizeof(current_path));
        }
        ret = json.write("current_path", current_path);
        if (ret != ESP_OK) return ret;

        // Get loaded image status
        loaded_image_status_t image_status = {0};
        bool have_image_status = false;
        if (host_comm && host_comm->initialized) {
            have_image_status = (host_comm_get_loaded_image_status(host_comm, &image_status) == ESP_OK);
        }

        if (have_image_status && image_status.image_loaded) {
            ret = json.write("current_image", image_status.image_name);
            if (ret != ESP_OK) return ret;
            ret = json.write("image_index", image_status.image_index);
            if (ret != ESP_OK) return ret;
            ret = json.write("total_images", image_status.total_images);
            if (ret != ESP_OK) return ret;
        } else {
            ret = json.write("current_image", (const char*)nullptr);
            if (ret != ESP_OK) return ret;
            ret = json.write("image_index", 0);
            if (ret != ESP_OK) return ret;
            ret = json.write("total_images", 0);
            if (ret != ESP_OK) return ret;
        }

        // Start entries array
        ret = json.writeKey("entries");
        if (ret != ESP_OK) return ret;

        ret = json.beginArray();
        if (ret != ESP_OK) return ret;

        // Stream entries one at a time to avoid stack overflow
        uint32_t entry_count = 0;
        if (host_comm && host_comm->initialized) {
            host_comm_get_entry_count(host_comm, &entry_count);
        }

        for (uint32_t i = 0; i < entry_count && i < 64; i++) {
            dir_entry_info_t entry;
            if (host_comm_get_entry_info(host_comm, i, &entry) == ESP_OK) {
                ret = json.beginObject();
                if (ret != ESP_OK) return ret;

                ret = json.write("name", entry.name);
                if (ret != ESP_OK) return ret;

                ret = json.write("is_directory", entry.entry_type == 0);
                if (ret != ESP_OK) return ret;

                ret = json.write("index", (int)i);
                if (ret != ESP_OK) return ret;

                ret = json.endObject();
                if (ret != ESP_OK) return ret;
            }
        }

        ret = json.endArray();
        if (ret != ESP_OK) return ret;
    } else {
        ret = json.write("current_path", "/");
        if (ret != ESP_OK) return ret;

        ret = json.write("current_image", (const char*)nullptr);
        if (ret != ESP_OK) return ret;

        ret = json.write("image_index", 0);
        if (ret != ESP_OK) return ret;

        ret = json.write("total_images", 0);
        if (ret != ESP_OK) return ret;

        ret = json.writeKey("entries");
        if (ret != ESP_OK) return ret;

        ret = json.beginArray();
        if (ret != ESP_OK) return ret;

        ret = json.endArray();
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_select_entry_handler(httpd_req_t *req) {
    // Read request body
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse JSON
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *entry_index_json = cJSON_GetObjectItem(json, "index");
    if (!cJSON_IsNumber(entry_index_json)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid index");
        return ESP_FAIL;
    }

    int32_t entry_index = (int32_t)entry_index_json->valueint;

    // Send command to host
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json_writer(req);
    esp_err_t json_ret = json_writer.beginObject();
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    bool success = false;
    if (g_server && g_server->interface_ctx) {
        esp_err_t select_ret = interface_select_entry(g_server->interface_ctx, entry_index);
        success = (select_ret == ESP_OK);
        if (!success) {
            json_ret = json_writer.write("error", esp_err_to_name(select_ret));
            if (json_ret != ESP_OK) {
                cJSON_Delete(json);
                return json_ret;
            }
        }
    } else {
        json_ret = json_writer.write("error", "Interface not available");
        if (json_ret != ESP_OK) {
            cJSON_Delete(json);
            return json_ret;
        }
    }

    json_ret = json_writer.write("success", success);
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    json_ret = json_writer.endObject();
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    cJSON_Delete(json);
    return json_writer.finalize();
}

static esp_err_t api_eject_image_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx) {
        esp_err_t eject_ret = interface_eject_image(g_server->interface_ctx);
        success = (eject_ret == ESP_OK);
        if (!success) {
            ret = json.write("error", esp_err_to_name(eject_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.write("success", success ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_prev_image_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t prev_ret = host_comm_select_prev_image(g_server->interface_ctx->host_comm);
        success = (prev_ret == ESP_OK);
        if (!success) {
            ret = json.write("error", esp_err_to_name(prev_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.write("success", success ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_next_image_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t next_ret = host_comm_select_next_image(g_server->interface_ctx->host_comm);
        success = (next_ret == ESP_OK);
        if (!success) {
            ret = json.write("error", esp_err_to_name(next_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.write("success", success ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_wifi_scan_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx && g_server->interface_ctx->wifi_manager) {
        // Start WiFi scan
        esp_err_t scan_ret = wifi_manager_scan_start(g_server->interface_ctx->wifi_manager);

        if (scan_ret != ESP_OK) {
            ret = json.write("error", esp_err_to_name(scan_ret));
            if (ret != ESP_OK) return ret;
        }

        // Get number of APs found
        uint16_t ap_count = 0;
        scan_ret = wifi_manager_scan_get_count(g_server->interface_ctx->wifi_manager, &ap_count);

        if (scan_ret != ESP_OK) {
            ret = json.write("error", esp_err_to_name(scan_ret));
            if (ret != ESP_OK) return ret;
        }

        ret = json.writeKey("networks");
        if (ret != ESP_OK) return ret;

        ret = json.beginArray();
        if (ret != ESP_OK) return ret;

        // Fetch and stream each AP record one at a time
        for (uint16_t i = 0; i < ap_count && i < 32; i++) {
            wifi_ap_info_t ap_info;
            scan_ret = wifi_manager_scan_get_ap(g_server->interface_ctx->wifi_manager, &ap_info);

            if (scan_ret == ESP_OK) {
                ret = json.beginObject();
                if (ret != ESP_OK) {
                    // Clean up remaining APs before returning
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }

                ret = json.write("ssid", ap_info.ssid);
                if (ret != ESP_OK) {
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }

                ret = json.write("rssi", ap_info.rssi);
                if (ret != ESP_OK) {
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }

                ret = json.write("auth_mode", interface_auth_mode_string(ap_info.auth_mode));
                if (ret != ESP_OK) {
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }

                ret = json.write("has_password", ap_info.has_password ? 1 : 0);
                if (ret != ESP_OK) {
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }

                ret = json.endObject();
                if (ret != ESP_OK) {
                    wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);
                    return ret;
                }
            }
        }

        // Clean up any remaining AP records (if ap_count > 32)
        wifi_manager_scan_cleanup(g_server->interface_ctx->wifi_manager);

        ret = json.endArray();
        if (ret != ESP_OK) return ret;
    } else {
        // Add mock data when interface is not available
        ret = json.writeKey("networks");
        if (ret != ESP_OK) return ret;

        ret = json.beginArray();
        if (ret != ESP_OK) return ret;

        ret = json.beginObject();
        if (ret != ESP_OK) return ret;

        ret = json.write("ssid", "Example Network");
        if (ret != ESP_OK) return ret;

        ret = json.write("rssi", -45);
        if (ret != ESP_OK) return ret;

        ret = json.write("auth_mode", "WPA2");
        if (ret != ESP_OK) return ret;

        ret = json.write("has_password", 1);
        if (ret != ESP_OK) return ret;

        ret = json.endObject();
        if (ret != ESP_OK) return ret;

        ret = json.endArray();
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_wifi_connect_handler(httpd_req_t *req) {
    // Read request body
    char content[256];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse JSON
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_json = cJSON_GetObjectItem(json, "password");

    if (!cJSON_IsString(ssid_json)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid SSID");
        return ESP_FAIL;
    }

    const char *ssid = ssid_json->valuestring;
    const char *password = (cJSON_IsString(password_json)) ? password_json->valuestring : NULL;

    // Connect to WiFi
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json_writer(req);
    esp_err_t json_ret = json_writer.beginObject();
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    bool success = false;
    if (g_server && g_server->interface_ctx) {
        esp_err_t connect_ret = interface_wifi_connect(g_server->interface_ctx, ssid, password);
        success = (connect_ret == ESP_OK);
        if (!success) {
            json_ret = json_writer.write("error", esp_err_to_name(connect_ret));
            if (json_ret != ESP_OK) {
                cJSON_Delete(json);
                return json_ret;
            }
        }
    } else {
        json_ret = json_writer.write("error", "Interface not available");
        if (json_ret != ESP_OK) {
            cJSON_Delete(json);
            return json_ret;
        }
    }

    json_ret = json_writer.write("success", success);
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    json_ret = json_writer.endObject();
    if (json_ret != ESP_OK) {
        cJSON_Delete(json);
        return json_ret;
    }

    cJSON_Delete(json);
    return json_writer.finalize();
}

static esp_err_t api_wifi_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx) {
        wifi_manager_state_t state;
        char ip_address[16] = {0};
        esp_err_t wifi_ret = interface_wifi_get_status(g_server->interface_ctx, &state, ip_address, sizeof(ip_address));

        if (wifi_ret == ESP_OK) {
            ret = json.write("state", interface_wifi_state_string(state));
            if (ret != ESP_OK) return ret;

            ret = json.write("ip_address", ip_address);
            if (ret != ESP_OK) return ret;
        } else {
            ret = json.write("state", "Error");
            if (ret != ESP_OK) return ret;

            ret = json.write("ip_address", "");
            if (ret != ESP_OK) return ret;

            ret = json.write("error", esp_err_to_name(wifi_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("state", "Unavailable");
        if (ret != ESP_OK) return ret;

        ret = json.write("ip_address", "");
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

esp_err_t web_server_init(web_server_t *server, uint16_t port) {
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(server, 0, sizeof(web_server_t));
    server->port = (port > 0) ? port : WEB_SERVER_PORT;
    server->initialized = true;
    g_server = server;

    ESP_LOGI(TAG, "Web server initialized on port %d", server->port);
    return ESP_OK;
}

esp_err_t web_server_start(web_server_t *server) {
    if (!server || !server->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (server->running) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = server->port;
    config.max_uri_handlers = WEB_SERVER_MAX_HANDLERS;

    esp_err_t ret = httpd_start(&server->server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    for (int i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ret = httpd_register_uri_handler(server->server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler for %s: %s",
                     uri_handlers[i].uri, esp_err_to_name(ret));
            httpd_stop(server->server);
            return ret;
        }
    }

    server->running = true;
    ESP_LOGI(TAG, "Web server started on port %d", server->port);
    return ESP_OK;
}

esp_err_t web_server_stop(web_server_t *server) {
    if (!server || !server->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!server->running) {
        ESP_LOGW(TAG, "Web server not running");
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(server->server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
        return ret;
    }

    server->server = NULL;
    server->running = false;
    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}

esp_err_t web_server_deinit(web_server_t *server) {
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    if (server->running) {
        web_server_stop(server);
    }

    server->initialized = false;
    if (g_server == server) {
        g_server = NULL;
    }

    ESP_LOGI(TAG, "Web server deinitialized");
    return ESP_OK;
}

esp_err_t web_server_set_interface_context(web_server_t *server, interface_context_t *interface_ctx) {
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    server->interface_ctx = interface_ctx;
    return ESP_OK;
}

bool web_server_is_running(web_server_t *server) {
    if (!server) {
        return false;
    }
    return server->running;
}

uint16_t web_server_get_port(web_server_t *server) {
    if (!server) {
        return 0;
    }
    return server->port;
}

static esp_err_t api_firmware_check_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    // Initialize OTA manager if needed
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        ota_manager_init(&g_ota_manager, g_server->interface_ctx->host_comm);

        // Get current version
        uint32_t current_version = ota_manager_get_current_version();
        ret = json.write("current_version", current_version);
        if (ret != ESP_OK) return ret;

        // Check for update
        bool update_available = false;
        esp_err_t check_ret = ota_manager_check_update(&g_ota_manager, &update_available);

        if (check_ret == ESP_OK) {
            ret = json.write("update_available", update_available ? 1 : 0);
            if (ret != ESP_OK) return ret;

            if (update_available) {
                ret = json.write("available_version", g_ota_manager.firmware_info.version);
                if (ret != ESP_OK) return ret;

                ret = json.write("firmware_size", g_ota_manager.firmware_info.size);
                if (ret != ESP_OK) return ret;
            }
        } else {
            ret = json.write("update_available", 0);
            if (ret != ESP_OK) return ret;

            ret = json.write("error", esp_err_to_name(check_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("current_version", 0x00010000);
        if (ret != ESP_OK) return ret;

        ret = json.write("update_available", 0);
        if (ret != ESP_OK) return ret;

        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_firmware_update_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t update_ret = ota_manager_start_update(&g_ota_manager);
        success = (update_ret == ESP_OK);

        if (!success) {
            ret = json.write("error", esp_err_to_name(update_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.write("success", success ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_firmware_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    // Process OTA manager to update state
    ota_manager_process(&g_ota_manager);

    // Get current state
    ota_state_t state = ota_manager_get_state(&g_ota_manager);
    uint8_t progress = ota_manager_get_progress(&g_ota_manager);

    // Convert state to string
    const char* state_str;
    switch (state) {
        case OTA_STATE_IDLE:
            state_str = "idle";
            break;
        case OTA_STATE_CHECKING:
            state_str = "checking";
            break;
        case OTA_STATE_DOWNLOADING:
            state_str = "downloading";
            break;
        case OTA_STATE_VERIFYING:
            state_str = "verifying";
            break;
        case OTA_STATE_APPLYING:
            state_str = "applying";
            break;
        case OTA_STATE_SUCCESS:
            state_str = "success";
            break;
        case OTA_STATE_ERROR:
            state_str = "error";
            break;
        default:
            state_str = "unknown";
    }

    ret = json.write("state", state_str);
    if (ret != ESP_OK) return ret;

    ret = json.write("progress", progress);
    if (ret != ESP_OK) return ret;

    if (state == OTA_STATE_ERROR) {
        ret = json.write("error", esp_err_to_name(g_ota_manager.last_error));
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

// Mainboard (RP2350) firmware tracking state
static bool g_mainboard_update_started = false;

static esp_err_t api_mainboard_firmware_check_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        rp2350_fw_status_t status;
        esp_err_t check_ret = host_comm_get_rp2350_fw_status(g_server->interface_ctx->host_comm, &status);

        if (check_ret == ESP_OK) {
            ret = json.write("current_version", status.current_version);
            if (ret != ESP_OK) return ret;

            bool update_available = (status.available_version != 0);
            ret = json.write("update_available", update_available ? 1 : 0);
            if (ret != ESP_OK) return ret;

            if (update_available) {
                ret = json.write("available_version", status.available_version);
                if (ret != ESP_OK) return ret;
            }
        } else {
            ret = json.write("current_version", 0);
            if (ret != ESP_OK) return ret;

            ret = json.write("update_available", 0);
            if (ret != ESP_OK) return ret;

            ret = json.write("error", esp_err_to_name(check_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("current_version", 0);
        if (ret != ESP_OK) return ret;

        ret = json.write("update_available", 0);
        if (ret != ESP_OK) return ret;

        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_mainboard_firmware_update_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t update_ret = host_comm_start_rp2350_update(g_server->interface_ctx->host_comm);
        success = (update_ret == ESP_OK);

        if (success) {
            g_mainboard_update_started = true;
        } else {
            ret = json.write("error", esp_err_to_name(update_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.write("success", success ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_mainboard_firmware_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        panel_command_status_t cmd_status;
        esp_err_t status_ret = host_comm_get_command_status(g_server->interface_ctx->host_comm, &cmd_status);

        if (status_ret == ESP_OK) {
            // Determine state string based on async state
            const char* state_str;
            switch (cmd_status.state) {
                case PANEL_ASYNC_IDLE:
                    state_str = g_mainboard_update_started ? "rebooting" : "idle";
                    break;
                case PANEL_ASYNC_PROCESSING:
                    state_str = "updating";
                    break;
                case PANEL_ASYNC_READY:
                    state_str = "success";
                    g_mainboard_update_started = false;
                    break;
                case PANEL_ASYNC_ERROR:
                    state_str = "error";
                    g_mainboard_update_started = false;
                    break;
                default:
                    state_str = "unknown";
            }

            ret = json.write("state", state_str);
            if (ret != ESP_OK) return ret;

            ret = json.write("progress", cmd_status.progress);
            if (ret != ESP_OK) return ret;

            if (cmd_status.state == PANEL_ASYNC_ERROR) {
                ret = json.write("error", "Update failed");
                if (ret != ESP_OK) return ret;
            }
        } else {
            // Communication lost - board may be rebooting
            if (g_mainboard_update_started) {
                ret = json.write("state", "rebooting");
                if (ret != ESP_OK) return ret;

                ret = json.write("progress", 100);
                if (ret != ESP_OK) return ret;
            } else {
                ret = json.write("state", "error");
                if (ret != ESP_OK) return ret;

                ret = json.write("progress", 0);
                if (ret != ESP_OK) return ret;

                ret = json.write("error", "Communication lost");
                if (ret != ESP_OK) return ret;
            }
        }
    } else {
        ret = json.write("state", "error");
        if (ret != ESP_OK) return ret;

        ret = json.write("progress", 0);
        if (ret != ESP_OK) return ret;

        ret = json.write("error", "Interface not available");
        if (ret != ESP_OK) return ret;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

// File upload state
typedef struct {
    char filename[256];
    char target_path[512];
    uint32_t total_size;
    uint32_t bytes_received;
    bool upload_started;
    bool upload_finished;
} upload_state_t;

static upload_state_t g_upload_state = {
    .filename = {0},
    .target_path = {0},
    .total_size = 0,
    .bytes_received = 0,
    .upload_started = false,
    .upload_finished = false
};

static inline char* strnmem(const void* haystack, const char* needle, size_t haystacklen) {
    return (char *)memmem(haystack, haystacklen, needle, strlen(needle));
}

static esp_err_t api_upload_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Upload request received");

    // Reset upload state
    memset(&g_upload_state, 0, sizeof(g_upload_state));

    // Get content length
    size_t content_length = req->content_len;
    if (content_length == 0) {
        ESP_LOGE(TAG, "No content in upload request");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "No content", 10);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload content length: %u bytes", content_length);

    // Allocate buffer for reading chunks (match protocol chunk size)
    const size_t chunk_size = PANEL_FILE_CHUNK_SIZE;
    char *chunk_buffer = (char*)malloc(chunk_size);
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "Failed to allocate upload buffer");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Memory allocation failed", 23);
        return ESP_FAIL;
    }

    // Variables for parsing multipart data
    bool found_filename = false;
    bool found_file_size = false;
    uint32_t actual_file_size = 0;
    uint32_t file_data_start = 0;

    // Read first chunk to parse headers
    int received = httpd_req_recv(req, chunk_buffer, chunk_size);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive upload data");
        free(chunk_buffer);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Failed to receive data", 22);
        return ESP_FAIL;
    }

    // Parse multipart data - look for fileSize field first
    char *filesize_field = strnmem(chunk_buffer, "name=\"fileSize\"", received);
    if (filesize_field) {
        // Find the value after the field header
        char *value_start = strnmem(filesize_field, "\r\n\r\n", received - (filesize_field - chunk_buffer));
        if (value_start) {
            value_start += 4; // Skip \r\n\r\n
            char *value_end = strnmem(value_start, "\r\n--", received - (value_start - chunk_buffer));
            if (value_end) {
                char size_str[32] = {0};
                size_t size_len = value_end - value_start;
                if (size_len < sizeof(size_str)) {
                    strncpy(size_str, value_start, size_len);
                    actual_file_size = strtoul(size_str, NULL, 10);
                    found_file_size = true;
                    ESP_LOGI(TAG, "Found file size: %u bytes", actual_file_size);
                }
            }
        }
    }

    // Look for filename in the file data field
    char *filename_start = NULL;
    char *filedata_field = strnmem(chunk_buffer, "name=\"fileData\"", received);
    if (filedata_field) {
        filename_start = strnmem(filedata_field, "filename=\"", received - (filedata_field - chunk_buffer));
    }
    if (filename_start) {
        filename_start += 10; // Skip 'filename="'
        char *filename_end = strchr(filename_start, '"');
        if (filename_end) {
            size_t filename_len = filename_end - filename_start;
            if (filename_len < sizeof(g_upload_state.filename)) {
                strncpy(g_upload_state.filename, filename_start, filename_len);
                g_upload_state.filename[filename_len] = '\0';
                found_filename = true;
                ESP_LOGI(TAG, "Found filename: %s", g_upload_state.filename);
            }
        }
    }

    if (!found_filename) {
        ESP_LOGE(TAG, "Could not parse filename from upload");
        free(chunk_buffer);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid multipart data - missing filename", 40);
        return ESP_FAIL;
    }

    if (!found_file_size) {
        ESP_LOGE(TAG, "Could not parse file size from upload");
        free(chunk_buffer);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid multipart data - missing file size", 41);
        return ESP_FAIL;
    }

    // Set target path
    snprintf(g_upload_state.target_path, sizeof(g_upload_state.target_path),
             "/uploads/%s", g_upload_state.filename);

    // Look for start of file data (after the file field headers)
    if (filename_start) {
        // Now search for \r\n\r\n after filename_start
        char *data_start = strnmem(filename_start, "\r\n\r\n", received - (filename_start - chunk_buffer));
        if (data_start) {
            data_start += 4; // Skip \r\n\r\n
            file_data_start = data_start - chunk_buffer;
            ESP_LOGI(TAG, "File data starts at offset: %u", file_data_start);
        }
        // ESP_LOGI(TAG, "buffer starting at data_start: %c\n", tmp);
    }

    // Start file upload to RP2350
    if (!g_server || !g_server->interface_ctx->host_comm) {
        ESP_LOGE(TAG, "Host communication not available");
        free(chunk_buffer);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Host not connected", 18);
        return ESP_FAIL;
    }

    host_comm_t *host_comm = g_server->interface_ctx->host_comm;

    // Start file upload on RP2350 with actual file size
    esp_err_t ret = host_comm_start_file_upload(host_comm, g_upload_state.filename, actual_file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file upload: %s", esp_err_to_name(ret));
        free(chunk_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to start upload", 21);
        return ESP_FAIL;
    }

    // Process file data from the first chunk
    if (file_data_start > 0 && file_data_start < received) {
        size_t first_chunk_data_size = received - file_data_start;
        ESP_LOGI(TAG, "Processing first chunk data: %d bytes", first_chunk_data_size);

        // Since we're now reading in 256-byte chunks, this should always fit
        // But let's be safe and check anyway
        if (first_chunk_data_size > PANEL_FILE_CHUNK_SIZE) {
            ESP_LOGE(TAG, "First chunk data too large: %d bytes (max %d)",
                     first_chunk_data_size, PANEL_FILE_CHUNK_SIZE);
            free(chunk_buffer);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "Multipart header too large", 26);
            return ESP_FAIL;
        }

        ret = host_comm_write_file_chunk(host_comm,
                                        (uint8_t*)(chunk_buffer + file_data_start),
                                        first_chunk_data_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write first chunk: %s", esp_err_to_name(ret));
            free(chunk_buffer);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "Upload failed", 13);
            return ESP_FAIL;
        }

        g_upload_state.bytes_received += first_chunk_data_size;
    }

    // Calculate how much file data remains after first chunk
    size_t file_data_sent = (file_data_start > 0 && file_data_start < received) ?
                           (received - file_data_start) : 0;
    size_t remaining = actual_file_size - file_data_sent;
    while (remaining > 0) {
        size_t to_read = (remaining > chunk_size) ? chunk_size : remaining;

        int chunk_received = httpd_req_recv(req, chunk_buffer, to_read);
        if (chunk_received <= 0) {
            ESP_LOGE(TAG, "Failed to receive chunk data");
            free(chunk_buffer);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "Failed to receive data", 22);
            return ESP_FAIL;
        }

        // Write chunk to RP2350
        ret = host_comm_write_file_chunk(host_comm, (uint8_t*)chunk_buffer, chunk_received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write chunk: %s", esp_err_to_name(ret));
            free(chunk_buffer);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "Upload failed", 13);
            return ESP_FAIL;
        }

        g_upload_state.bytes_received += chunk_received;

        remaining -= chunk_received;

        ESP_LOGD(TAG, "Uploaded %u / %u bytes", g_upload_state.bytes_received, actual_file_size);
    }

    // Finish the upload and get file hash
    uint8_t upload_result;
    uint8_t file_hash[32];
    ret = host_comm_finish_file_upload(host_comm, &upload_result, file_hash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to finish upload: %s", esp_err_to_name(ret));
        free(chunk_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Upload failed", 13);
        return ESP_FAIL;
    }

    free(chunk_buffer);

    // Check upload result
    if (upload_result != PANEL_UPLOAD_OK) {
        ESP_LOGE(TAG, "Upload failed on RP2350 with code: 0x%02X", upload_result);
        httpd_resp_set_status(req, "500 Internal Server Error");

        const char* error_msg;
        switch (upload_result) {
            case PANEL_UPLOAD_ERROR_DISK:
                error_msg = "Disk error";
                break;
            case PANEL_UPLOAD_ERROR_SPACE:
                error_msg = "Insufficient space";
                break;
            case PANEL_UPLOAD_ERROR_WRITE:
                error_msg = "Write error";
                break;
            case PANEL_UPLOAD_ERROR_PATH:
                error_msg = "Invalid path";
                break;
            default:
                error_msg = "Unknown error";
        }

        httpd_resp_send(req, error_msg, strlen(error_msg));
        return ESP_FAIL;
    }

    // Convert hash to hex string
    char hash_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hash_hex + (i * 2), sizeof(hash_hex) - (i * 2), "%02x", file_hash[i]);
    }
    hash_hex[64] = '\0';

    ESP_LOGI(TAG, "File upload completed successfully: %s (%u bytes), SHA256: %s",
             g_upload_state.filename, g_upload_state.bytes_received, hash_hex);

    // Send success response with hash
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    ret = json.write("success", true);
    if (ret != ESP_OK) return ret;

    ret = json.write("filename", g_upload_state.filename);
    if (ret != ESP_OK) return ret;

    ret = json.write("path", g_upload_state.target_path);
    if (ret != ESP_OK) return ret;

    ret = json.write("size", (int)g_upload_state.bytes_received);
    if (ret != ESP_OK) return ret;

    ret = json.write("hash", hash_hex);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}
