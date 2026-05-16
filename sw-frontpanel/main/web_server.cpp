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
#include <inttypes.h>
#include <sys/param.h>  // MIN()
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include "interface_common.h"
#include "json_stream.h"
#include "ota_manager.h"
#include "wifi_manager.h"

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
#ifdef CONFIG_PRODUCT_BLUESCSI
static esp_err_t api_mainboard_firmware_check_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_update_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_status_handler(httpd_req_t *req);
static esp_err_t api_mainboard_firmware_upload_handler(httpd_req_t *req);
#endif
static esp_err_t api_devices_handler(httpd_req_t *req);
static esp_err_t api_upload_handler(httpd_req_t *req);
static esp_err_t api_download_handler(httpd_req_t *req);

// Global server instance for handlers
static web_server_t *g_server = NULL;

// Helper: extract device index from optional JSON body, falling back to active_device_index
static uint16_t get_device_from_body(httpd_req_t *req) {
    uint16_t default_device = (g_server && g_server->interface_ctx)
        ? g_server->interface_ctx->active_device_index : 0;
    if (req->content_len == 0 || req->content_len > 256) return default_device;
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return default_device;
    buf[len] = '\0';
    cJSON *json = cJSON_Parse(buf);
    if (!json) return default_device;
    cJSON *dev = cJSON_GetObjectItem(json, "device");
    uint16_t result = default_device;
    if (cJSON_IsNumber(dev)) result = (uint16_t)dev->valueint;
    cJSON_Delete(json);
    return result;
}

// Global OTA manager instance
static ota_manager_t g_ota_manager;
static TaskHandle_t g_ota_task_handle = NULL;

#ifdef CONFIG_PRODUCT_BLUESCSI
// Background OTA task - processes OTA update without blocking HTTP handler
static void web_firmware_update_task(void *pvParameters) {
    ota_manager_t *ota = (ota_manager_t *)pvParameters;

    while (1) {
        esp_err_t ret = ota_manager_process(ota);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            ESP_LOGE(TAG, "OTA process error: %s", esp_err_to_name(ret));
            break;
        }

        ota_state_t state = ota_manager_get_state(ota);

        if (state == OTA_STATE_SUCCESS) {
            ESP_LOGI(TAG, "Web OTA update successful, restarting in 3 seconds...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else if (state == OTA_STATE_ERROR) {
            ESP_LOGE(TAG, "Web OTA update failed");
            break;
        }

        taskYIELD();
    }

    g_ota_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

// Embedded file content (gzipped, symbols created by CMakeLists.txt)
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t sha256_js_gz_start[] asm("_binary_sha256_js_gz_start");
extern const uint8_t sha256_js_gz_end[] asm("_binary_sha256_js_gz_end");
extern const uint8_t logo_svg_gz_start[] asm("_binary_logo_svg_gz_start");
extern const uint8_t logo_svg_gz_end[] asm("_binary_logo_svg_gz_end");

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
static const static_file_t file_logo_svg = {
    logo_svg_gz_start, logo_svg_gz_end, "image/svg+xml"
};

// URI handlers
static const httpd_uri_t uri_handlers[] = {
    { .uri = "/", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_index_html },
    { .uri = "/app.js", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_app_js },
    { .uri = "/sha256.js", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_sha256_js },
    { .uri = "/logo.svg", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&file_logo_svg },
    { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL },
    { .uri = "/api/devices", .method = HTTP_GET, .handler = api_devices_handler, .user_ctx = NULL },
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
#ifdef CONFIG_PRODUCT_BLUESCSI
    { .uri = "/api/firmware/mainboard/check", .method = HTTP_GET, .handler = api_mainboard_firmware_check_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/update", .method = HTTP_POST, .handler = api_mainboard_firmware_update_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/status", .method = HTTP_GET, .handler = api_mainboard_firmware_status_handler, .user_ctx = NULL },
    { .uri = "/api/firmware/mainboard/upload", .method = HTTP_POST, .handler = api_mainboard_firmware_upload_handler, .user_ctx = NULL },
#endif
    { .uri = "/api/upload", .method = HTTP_POST, .handler = api_upload_handler, .user_ctx = NULL },
    { .uri = "/api/download", .method = HTTP_GET, .handler = api_download_handler, .user_ctx = NULL }
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

    ret = json.write("product_name", PRODUCT_NAME_SHORT);
    if (ret != ESP_OK) return ret;
    ret = json.write("product_full", PRODUCT_NAME_FULL);
    if (ret != ESP_OK) return ret;
    ret = json.write("hostname", WIFI_MANAGER_MDNS_HOSTNAME);
    if (ret != ESP_OK) return ret;
    ret = json.write("logo_url", PRODUCT_LOGO_URL);
    if (ret != ESP_OK) return ret;

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}

static esp_err_t api_devices_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx) {
        uint16_t active_device = g_server->interface_ctx->active_device_index;
        ret = json.write("active_device", (int)active_device);
        if (ret != ESP_OK) return ret;

        // Get device list from host
        uint8_t buf[sizeof(device_list_response_t) + CONFIG_MAX_DEVICES * sizeof(device_summary_t)];
        device_list_response_t *list = (device_list_response_t *)buf;

        esp_err_t list_ret = interface_get_device_list(g_server->interface_ctx, list, sizeof(buf));

        if (list_ret == ESP_OK) {
            ret = json.write("device_count", (int)list->device_count);
            if (ret != ESP_OK) return ret;

            ret = json.writeKey("devices");
            if (ret != ESP_OK) return ret;
            ret = json.beginArray();
            if (ret != ESP_OK) return ret;

            for (uint8_t i = 0; i < list->device_count; i++) {
                ret = json.beginObject();
                if (ret != ESP_OK) return ret;

                ret = json.write("index", (int)list->devices[i].device_index);
                if (ret != ESP_OK) return ret;
                ret = json.write("type", (int)list->devices[i].device_type);
                if (ret != ESP_OK) return ret;
                ret = json.write("status", (int)list->devices[i].device_status);
                if (ret != ESP_OK) return ret;
                ret = json.write("label", list->devices[i].device_label);
                if (ret != ESP_OK) return ret;
                ret = json.write("image", list->devices[i].image_name);
                if (ret != ESP_OK) return ret;

                ret = json.endObject();
                if (ret != ESP_OK) return ret;
            }

            ret = json.endArray();
            if (ret != ESP_OK) return ret;
        } else {
            ret = json.write("device_count", 0);
            if (ret != ESP_OK) return ret;
            ret = json.writeKey("devices");
            if (ret != ESP_OK) return ret;
            ret = json.beginArray();
            if (ret != ESP_OK) return ret;
            ret = json.endArray();
            if (ret != ESP_OK) return ret;
            ret = json.write("error", esp_err_to_name(list_ret));
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("active_device", 0);
        if (ret != ESP_OK) return ret;
        ret = json.write("device_count", 0);
        if (ret != ESP_OK) return ret;
        ret = json.writeKey("devices");
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

        // Get device index from query parameter or use active device
        uint16_t device_index = g_server->interface_ctx->active_device_index;
        {
            size_t query_len = httpd_req_get_url_query_len(req);
            if (query_len > 0 && query_len < 64) {
                char query[64];
                if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
                    char device_str[8];
                    if (httpd_query_key_value(query, "device", device_str, sizeof(device_str)) == ESP_OK) {
                        device_index = (uint16_t)atoi(device_str);
                    }
                }
            }
        }

        // Get loaded image status
        loaded_image_status_t image_status = {0};
        bool have_image_status = false;
        if (host_comm && host_comm->initialized) {
            have_image_status = (host_comm_get_loaded_image_status(host_comm, device_index, &image_status) == ESP_OK);
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

    // Parse optional device field, falling back to active_device_index
    cJSON *device_json = cJSON_GetObjectItem(json, "device");
    uint16_t device_index = (g_server && g_server->interface_ctx)
        ? g_server->interface_ctx->active_device_index : 0;
    if (cJSON_IsNumber(device_json)) device_index = (uint16_t)device_json->valueint;

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
        esp_err_t select_ret = interface_select_entry(g_server->interface_ctx, entry_index, device_index);
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
    uint16_t device_index = get_device_from_body(req);

    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx) {
        esp_err_t eject_ret = interface_eject_image(g_server->interface_ctx, device_index);
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
    uint16_t device_index = get_device_from_body(req);

    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t prev_ret = host_comm_select_prev_image(g_server->interface_ctx->host_comm, device_index);
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
    uint16_t device_index = get_device_from_body(req);

    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t next_ret = host_comm_select_next_image(g_server->interface_ctx->host_comm, device_index);
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

    if (g_server && g_server->interface_ctx && g_server->interface_ctx->wifi_manager) {
        wifi_manager_t *wifi = g_server->interface_ctx->wifi_manager;
        wifi_manager_state_t state = wifi_manager_get_state(wifi);

        ret = json.write("state", interface_wifi_state_string(state));
        if (ret != ESP_OK) return ret;

        // Determine mode: AP, Client, or None
        const char *mode = "None";
        if (wifi->ap_active) {
            mode = "AP";
        } else if (wifi->station_connected) {
            mode = "Client";
        }
        ret = json.write("mode", mode);
        if (ret != ESP_OK) return ret;

        // Include SSID based on mode
        if (wifi->station_connected) {
            ret = json.write("ssid", wifi->config.ssid);
            if (ret != ESP_OK) return ret;

            esp_ip4_addr_t ip;
            if (wifi_manager_get_ip_info(wifi, &ip, NULL, NULL) == ESP_OK) {
                char ip_address[16];
                snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip));
                ret = json.write("ip_address", ip_address);
                if (ret != ESP_OK) return ret;
            } else {
                ret = json.write("ip_address", "");
                if (ret != ESP_OK) return ret;
            }
        } else if (wifi->ap_active) {
            ret = json.write("ssid", WIFI_MANAGER_AP_SSID);
            if (ret != ESP_OK) return ret;

            char ip_address[16];
            snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&wifi->ap_ip_addr));
            ret = json.write("ip_address", ip_address);
            if (ret != ESP_OK) return ret;
        } else {
            ret = json.write("ssid", "");
            if (ret != ESP_OK) return ret;

            ret = json.write("ip_address", "");
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = json.write("state", "Unavailable");
        if (ret != ESP_OK) return ret;

        ret = json.write("mode", "None");
        if (ret != ESP_OK) return ret;

        ret = json.write("ssid", "");
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
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;

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
#ifdef CONFIG_PRODUCT_BLUESCSI
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

            // Include firmware info if a file is present (even if same version)
            if (g_ota_manager.firmware_info.available) {
                ret = json.write("available_version", g_ota_manager.firmware_info.version);
                if (ret != ESP_OK) return ret;

                ret = json.write("firmware_size", g_ota_manager.firmware_info.size);
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
#else
        host_comm_t *comm = g_server->interface_ctx->host_comm;

        // Get main board firmware status (user-facing "system version")
        rp2350_fw_status_t rp_status = {};
        esp_err_t rp_ret = host_comm_get_rp2350_fw_status(comm, &rp_status);

        if (rp_ret == ESP_OK) {
            ret = json.write("current_version", rp_status.current_version);
            if (ret != ESP_OK) return ret;

            // Check if any update is available (main board or panel)
            bool main_update = (rp_status.available_version != 0);

            // Also check panel firmware from combined UF2
            ota_manager_init(&g_ota_manager, comm);
            bool panel_update = false;
            panel_firmware_info_t panel_info = {};
            esp_err_t panel_ret = host_comm_check_firmware(comm, &panel_info);
            if (panel_ret == ESP_OK && panel_info.available) {
                uint32_t current_panel_ver = ota_manager_get_current_version();
                panel_update = (panel_info.version > current_panel_ver);
            }

            bool any_update = main_update || panel_update;
            ret = json.write("update_available", any_update ? 1 : 0);
            if (ret != ESP_OK) return ret;

            if (any_update) {
                ret = json.write("available_version", rp_status.available_version);
                if (ret != ESP_OK) return ret;
            }
        } else {
            ret = json.write("current_version", 0);
            if (ret != ESP_OK) return ret;

            ret = json.write("update_available", 0);
            if (ret != ESP_OK) return ret;

            ret = json.write("error", esp_err_to_name(rp_ret));
            if (ret != ESP_OK) return ret;
        }
#endif
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

#ifndef CONFIG_PRODUCT_BLUESCSI
// Unified update state tracking for web UI
typedef enum {
    WEB_UPDATE_IDLE = 0,
    WEB_UPDATE_PANEL,
    WEB_UPDATE_MAINBOARD,
    WEB_UPDATE_WAITING_REBOOT,
    WEB_UPDATE_SUCCESS,
    WEB_UPDATE_ERROR
} web_update_phase_t;

static volatile web_update_phase_t g_web_update_phase = WEB_UPDATE_IDLE;
static volatile uint8_t g_web_update_progress = 0;
static const char* g_web_update_error = NULL;

// Background task that runs the unified update flow
static void web_firmware_update_task(void *pvParameters) {
    host_comm_t *comm = (host_comm_t *)pvParameters;

    // Check what needs updating
    panel_firmware_info_t panel_info = {};
    esp_err_t ret = host_comm_check_firmware(comm, &panel_info);
    bool panel_needs_update = (ret == ESP_OK && panel_info.available &&
                               panel_info.version > ota_manager_get_current_version());

    rp2350_fw_status_t rp_status = {};
    ret = host_comm_get_rp2350_fw_status(comm, &rp_status);
    bool main_needs_update = (ret == ESP_OK && rp_status.available_version != 0);

    bool panel_ota_written = false;

    // Phase 1: Panel OTA (if needed)
    if (panel_needs_update) {
        g_web_update_phase = WEB_UPDATE_PANEL;
        g_web_update_progress = 0;

        ota_manager_init(&g_ota_manager, comm);
        memcpy(&g_ota_manager.firmware_info, &panel_info, sizeof(panel_firmware_info_t));

        ret = ota_manager_start_update(&g_ota_manager);
        if (ret != ESP_OK) {
            g_web_update_error = "Failed to start panel OTA";
            g_web_update_phase = WEB_UPDATE_ERROR;
            vTaskDelete(NULL);
            return;
        }

        while (1) {
            ret = ota_manager_process(&g_ota_manager);
            ota_state_t state = ota_manager_get_state(&g_ota_manager);
            g_web_update_progress = ota_manager_get_progress(&g_ota_manager);

            if (state == OTA_STATE_SUCCESS) {
                panel_ota_written = true;
                break;
            } else if (state == OTA_STATE_ERROR) {
                g_web_update_error = "Panel OTA failed";
                g_web_update_phase = WEB_UPDATE_ERROR;
                vTaskDelete(NULL);
                return;
            }
            taskYIELD();
        }
    }

    // Phase 2: Main board update
    if (main_needs_update) {
        g_web_update_phase = WEB_UPDATE_MAINBOARD;
        g_web_update_progress = 0;

        ret = host_comm_start_rp2350_update(comm);
        if (ret != ESP_OK) {
            g_web_update_error = "Failed to start main board update";
            if (panel_ota_written) {
                // Panel OTA was already written, reboot to apply it at least
                esp_restart();
            }
            g_web_update_phase = WEB_UPDATE_ERROR;
            vTaskDelete(NULL);
            return;
        }

        // Wait for main board to reboot
        g_web_update_phase = WEB_UPDATE_WAITING_REBOOT;
        g_web_update_progress = 50;

        // Give it time to flash and reboot
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Poll for main board to come back
        for (int attempts = 0; attempts < 20; attempts++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            rp2350_fw_status_t new_status;
            if (host_comm_get_rp2350_fw_status(comm, &new_status) == ESP_OK) {
                g_web_update_progress = 100;
                break;
            }
        }
    }

    // Phase 3: Reboot panel if OTA was written
    if (panel_ota_written) {
        g_web_update_phase = WEB_UPDATE_SUCCESS;
        g_web_update_progress = 100;
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    g_web_update_phase = WEB_UPDATE_SUCCESS;
    g_web_update_progress = 100;
    vTaskDelete(NULL);
}
#endif

#ifdef CONFIG_PRODUCT_BLUESCSI
static esp_err_t api_firmware_update_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_ota_task_handle != NULL) {
        ret = json.write("error", "Update already in progress");
        if (ret != ESP_OK) return ret;
    } else if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm) {
        esp_err_t update_ret = ota_manager_start_update(&g_ota_manager);
        success = (update_ret == ESP_OK);

        if (success) {
            // Start background task to process OTA
            BaseType_t task_ret = xTaskCreate(web_firmware_update_task, "web_ota",
                                              8192, &g_ota_manager, 10, &g_ota_task_handle);
            if (task_ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create OTA task");
                ota_manager_abort(&g_ota_manager);
                success = false;
                ret = json.write("error", "Failed to create update task");
                if (ret != ESP_OK) return ret;
            }
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
#else
// Unified firmware update trigger
static esp_err_t api_firmware_update_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx && g_server->interface_ctx->host_comm &&
        g_web_update_phase == WEB_UPDATE_IDLE) {

        g_web_update_phase = WEB_UPDATE_PANEL;  // Will be adjusted by task
        g_web_update_progress = 0;
        g_web_update_error = NULL;

        BaseType_t task_ret = xTaskCreate(web_firmware_update_task, "web_fw_update", 8192,
                                          g_server->interface_ctx->host_comm, 10, NULL);
        success = (task_ret == pdPASS);

        if (!success) {
            g_web_update_phase = WEB_UPDATE_IDLE;
            ret = json.write("error", "Failed to create update task");
            if (ret != ESP_OK) return ret;
        }
    } else if (g_web_update_phase != WEB_UPDATE_IDLE) {
        ret = json.write("error", "Update already in progress");
        if (ret != ESP_OK) return ret;
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
#endif

#ifdef CONFIG_PRODUCT_BLUESCSI
static esp_err_t api_firmware_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    // OTA processing happens in background task (web_firmware_update_task),
    // so we just read the current state here without calling ota_manager_process.

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
#else
// Unified firmware status
static esp_err_t api_firmware_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    const char* state_str;
    switch (g_web_update_phase) {
        case WEB_UPDATE_IDLE:       state_str = "idle"; break;
        case WEB_UPDATE_PANEL:      state_str = "updating_panel"; break;
        case WEB_UPDATE_MAINBOARD:  state_str = "updating_mainboard"; break;
        case WEB_UPDATE_WAITING_REBOOT: state_str = "rebooting"; break;
        case WEB_UPDATE_SUCCESS:    state_str = "success"; break;
        case WEB_UPDATE_ERROR:      state_str = "error"; break;
        default:                    state_str = "unknown"; break;
    }

    ret = json.write("state", state_str);
    if (ret != ESP_OK) return ret;

    ret = json.write("progress", (uint32_t)g_web_update_progress);
    if (ret != ESP_OK) return ret;

    if (g_web_update_phase == WEB_UPDATE_ERROR && g_web_update_error) {
        ret = json.write("error", g_web_update_error);
        if (ret != ESP_OK) return ret;
    }

    // Reset to idle after client reads success/error
    if (g_web_update_phase == WEB_UPDATE_SUCCESS || g_web_update_phase == WEB_UPDATE_ERROR) {
        g_web_update_phase = WEB_UPDATE_IDLE;
    }

    ret = json.endObject();
    if (ret != ESP_OK) return ret;

    return json.finalize();
}
#endif

#ifdef CONFIG_PRODUCT_BLUESCSI
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

// For PicoIDE: main board firmware is read from SD card by the RP2350, updated via SPI command.
// For BlueSCSI: main board firmware is uploaded via web UI to SD card, then applied on reboot.
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
#endif // #ifdef CONFIG_PRODUCT_BLUESCSI

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

static esp_err_t handle_file_upload(httpd_req_t *req, const char *path_prefix) {
    ESP_LOGI(TAG, "Upload request received (prefix: \"%s\")", path_prefix);

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

    // Set target path using path_prefix + filename
    snprintf(g_upload_state.target_path, sizeof(g_upload_state.target_path),
             "%s%s", path_prefix, g_upload_state.filename);

    // Look for start of file data (after the file field headers)
    if (filename_start) {
        // Now search for \r\n\r\n after filename_start
        char *data_start = strnmem(filename_start, "\r\n\r\n", received - (filename_start - chunk_buffer));
        if (data_start) {
            data_start += 4; // Skip \r\n\r\n
            file_data_start = data_start - chunk_buffer;
            ESP_LOGI(TAG, "File data starts at offset: %u", file_data_start);
        }
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
    esp_err_t ret = host_comm_start_file_upload(host_comm, g_upload_state.target_path, actual_file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file upload: %s", esp_err_to_name(ret));
        free(chunk_buffer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to start upload", 21);
        return ESP_FAIL;
    }

    // Process file data from the first chunk
    uint32_t file_data_sent = 0;
    if (file_data_start > 0 && (uint32_t)file_data_start < (uint32_t)received) {
        size_t first_chunk_data_size = received - file_data_start;
        // Cap at actual file size (don't include trailing multipart boundary for small files)
        if (first_chunk_data_size > actual_file_size) {
            first_chunk_data_size = actual_file_size;
        }
        ESP_LOGI(TAG, "Processing first chunk data: %d bytes", first_chunk_data_size);

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

        file_data_sent = first_chunk_data_size;
        g_upload_state.bytes_received += first_chunk_data_size;
    }

    // Calculate how much file data remains after first chunk
    size_t remaining = actual_file_size - g_upload_state.bytes_received;
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
             g_upload_state.target_path, g_upload_state.bytes_received, hash_hex);

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

// Thin wrapper: regular file upload (JS sends full path in filename)
static esp_err_t api_upload_handler(httpd_req_t *req) {
    return handle_file_upload(req, "");
}

#ifdef CONFIG_PRODUCT_BLUESCSI
// Thin wrapper: mainboard firmware upload (prefix "/" to ensure root path)
static esp_err_t api_mainboard_firmware_upload_handler(httpd_req_t *req) {
    return handle_file_upload(req, "/");
}
#endif

// File download handler - downloads a file from the main board SD card
// Usage: GET /api/download?path=/picoide.ini
static esp_err_t api_download_handler(httpd_req_t *req) {
    if (!g_server || !g_server->interface_ctx || !g_server->interface_ctx->host_comm) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Host communication not available");
        return ESP_FAIL;
    }

    host_comm_t *host_comm = g_server->interface_ctx->host_comm;

    // Get the path query parameter
    char path[256] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_FAIL;
    }

    char *query_str = (char *)malloc(query_len + 1);
    if (!query_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_req_get_url_query_str(req, query_str, query_len + 1);
    if (ret != ESP_OK) {
        free(query_str);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to get query string");
        return ESP_FAIL;
    }

    ret = httpd_query_key_value(query_str, "path", path, sizeof(path));
    free(query_str);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid path parameter");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Download request for: %s", path);

    // Start the file download
    uint32_t file_size = 0;
    ret = host_comm_start_file_download(host_comm, path, &file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file download: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloading file: %s (%" PRIu32 " bytes)", path, file_size);

    // Set content type based on file extension
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".ini") == 0) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    // Set content length
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%" PRIu32, file_size);
    httpd_resp_set_hdr(req, "Content-Length", content_length);

    // Read and send file in chunks
    uint8_t *chunk_buffer = (uint8_t *)malloc(PANEL_FILE_CHUNK_SIZE);
    if (!chunk_buffer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    uint32_t bytes_sent = 0;
    uint32_t chunk_index = 0;

    while (bytes_sent < file_size) {
        size_t bytes_read = 0;
        ret = host_comm_read_file_chunk(host_comm, chunk_index, chunk_buffer, &bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read file chunk %" PRIu32 ": %s", chunk_index, esp_err_to_name(ret));
            free(chunk_buffer);
            return ESP_FAIL;
        }

        if (bytes_read == 0) {
            break;  // End of file
        }

        ret = httpd_resp_send_chunk(req, (const char *)chunk_buffer, bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk: %s", esp_err_to_name(ret));
            free(chunk_buffer);
            return ESP_FAIL;
        }

        bytes_sent += bytes_read;
        chunk_index++;
    }

    free(chunk_buffer);

    // Send final empty chunk to signal end of response
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "Download complete: %" PRIu32 " bytes sent", bytes_sent);
    return ESP_OK;
}
