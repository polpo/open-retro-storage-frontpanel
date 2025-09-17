#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include "interface_common.h"
#include "json_stream.h"

static const char *TAG = "web_server";

// Forward declarations
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t api_status_handler(httpd_req_t *req);
static esp_err_t api_discs_handler(httpd_req_t *req);
static esp_err_t api_select_disc_handler(httpd_req_t *req);
static esp_err_t api_eject_disc_handler(httpd_req_t *req);
static esp_err_t api_wifi_scan_handler(httpd_req_t *req);
static esp_err_t api_wifi_connect_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_handler(httpd_req_t *req);

// Global server instance for handlers
static web_server_t *g_server = NULL;

// HTML content for the web interface
static const char index_html[] = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>PicoIDE Front Panel</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f0f0; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        .header { text-align: center; margin-bottom: 30px; }
        .section { margin-bottom: 30px; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }
        .section h3 { margin-top: 0; color: #333; }
        button { background-color: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; margin: 5px; }
        button:hover { background-color: #0056b3; }
        button:disabled { background-color: #6c757d; cursor: not-allowed; }
        .disc-list { margin: 10px 0; }
        .disc-item { display: flex; justify-content: space-between; align-items: center; padding: 8px; border: 1px solid #ddd; margin: 2px 0; border-radius: 3px; }
        .status { padding: 10px; background-color: #e9ecef; border-radius: 4px; margin: 10px 0; }
        .error { background-color: #f8d7da; color: #721c24; }
        .success { background-color: #d4edda; color: #155724; }
        .wifi-list { margin: 10px 0; }
        .wifi-item { display: flex; justify-content: space-between; align-items: center; padding: 8px; border: 1px solid #ddd; margin: 2px 0; border-radius: 3px; }
        .signal-strength { font-size: 12px; color: #666; }
        input[type="text"], input[type="password"] { width: 200px; padding: 5px; margin: 5px; }
        .refresh-btn { background-color: #28a745; }
        .refresh-btn:hover { background-color: #1e7e34; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🐦 PicoIDE</h1>
            <p style="color: #666; margin: 5px 0;">Available at <strong>http://picoide.local</strong></p>
            <div id="connection-status" class="status">Connecting...</div>
        </div>

        <div class="section">
            <h3>💿 Disc Management</h3>
            <div id="current-disc" class="status">Current Disc: <span id="current-disc-name">Loading...</span></div>
            <button onclick="refreshDiscs()" class="refresh-btn">🔄 Refresh Disc List</button>
            <button onclick="ejectDisc()">⏏️ Eject Current Disc</button>
            <div id="disc-list" class="disc-list">
                <div>Loading disc list...</div>
            </div>
        </div>

        <div class="section">
            <h3>📶 WiFi Configuration</h3>
            <div id="wifi-status" class="status">WiFi Status: <span id="wifi-state">Unknown</span></div>
            <button onclick="scanWiFi()" class="refresh-btn">🔍 Scan Networks</button>
            <div id="wifi-list" class="wifi-list">
                <div>Click "Scan Networks" to see available WiFi networks</div>
            </div>
            <div style="margin-top: 15px;">
                <h4>Manual Connection</h4>
                <input type="text" id="manual-ssid" placeholder="Network Name (SSID)" />
                <input type="password" id="manual-password" placeholder="Password" />
                <button onclick="connectToWiFi(document.getElementById('manual-ssid').value, document.getElementById('manual-password').value)">🔗 Connect</button>
            </div>
        </div>

        <div class="section">
            <h3>ℹ️ System Information</h3>
            <div id="system-info" class="status">Loading system information...</div>
        </div>
    </div>

    <script>
        let currentDisc = "No disc loaded";

        // API helper function
        async function apiCall(endpoint, options = {}) {
            try {
                const response = await fetch('/api' + endpoint, {
                    headers: {
                        'Content-Type': 'application/json',
                        ...options.headers
                    },
                    ...options
                });

                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}: ${response.statusText}`);
                }

                return await response.json();
            } catch (error) {
                console.error('API call failed:', error);
                showStatus('error', `API Error: ${error.message}`);
                return null;
            }
        }

        function showStatus(type, message) {
            const statusDiv = document.getElementById('connection-status');
            statusDiv.className = `status ${type}`;
            statusDiv.textContent = message;
        }

        async function loadSystemInfo() {
            const data = await apiCall('/status');
            if (data) {
                document.getElementById('system-info').innerHTML = `
                    <strong>Firmware:</strong> ${data.firmware || 'v0.1.0'}<br>
                    <strong>Hardware:</strong> ${data.hardware || 'ESP32-C3'}<br>
                    <strong>Host Communication:</strong> ${data.transport || 'Unknown'} - ${data.host_connected ? 'Connected' : 'Disconnected'}<br>
                    <strong>Free Memory:</strong> ${data.free_memory || 'Unknown'} bytes<br>
                    <strong>Uptime:</strong> ${data.uptime || 'Unknown'} seconds
                `;
                showStatus('success', 'Connected to front panel');
            } else {
                showStatus('error', 'Failed to connect to front panel');
            }
        }

        async function refreshDiscs() {
            const data = await apiCall('/discs');
            if (data) {
                const listDiv = document.getElementById('disc-list');
                if (data.discs && data.discs.length > 0) {
                    listDiv.innerHTML = data.discs.map((disc, index) =>
                        `<div class="disc-item">
                            <span><strong>${disc.name}</strong> (${(disc.size / 1000000).toFixed(1)} MB, ${disc.tracks} tracks)</span>
                            <button onclick="selectDisc(${index})">📀 Select</button>
                        </div>`
                    ).join('');
                } else {
                    listDiv.innerHTML = '<div>No discs available</div>';
                }

                // Update current disc status
                document.getElementById('current-disc-name').textContent = data.current_disc || 'No disc loaded';
            }
        }

        async function selectDisc(index) {
            const data = await apiCall('/select_disc', {
                method: 'POST',
                body: JSON.stringify({ disc_index: index })
            });
            if (data && data.success) {
                await refreshDiscs();
                showStatus('success', `Selected disc ${index}`);
            }
        }

        async function ejectDisc() {
            const data = await apiCall('/eject_disc', { method: 'POST' });
            if (data && data.success) {
                await refreshDiscs();
                showStatus('success', 'Disc ejected');
            }
        }

        async function loadWiFiStatus() {
            const data = await apiCall('/wifi/status');
            if (data) {
                document.getElementById('wifi-state').textContent = data.state || 'Unknown';
                if (data.ip_address) {
                    document.getElementById('wifi-status').innerHTML = `WiFi Status: <strong>${data.state}</strong> (IP: ${data.ip_address})`;
                }
            }
        }

        async function scanWiFi() {
            showStatus('', 'Scanning WiFi networks...');
            const data = await apiCall('/wifi/scan');
            if (data && data.networks) {
                const listDiv = document.getElementById('wifi-list');
                if (data.networks.length > 0) {
                    listDiv.innerHTML = data.networks.map(network =>
                        `<div class="wifi-item">
                            <span><strong>${network.ssid}</strong> <span class="signal-strength">(${network.rssi} dBm, ${network.auth_mode})</span></span>
                            <button onclick="connectToWiFi('${network.ssid}', null, ${!network.has_password})">🔗 Connect</button>
                        </div>`
                    ).join('');
                } else {
                    listDiv.innerHTML = '<div>No networks found</div>';
                }
                showStatus('success', `Found ${data.networks.length} networks`);
            }
        }

        async function connectToWiFi(ssid, password, isOpen = false) {
            if (!isOpen && !password) {
                password = prompt(`Enter password for ${ssid}:`);
                if (!password) return;
            }

            showStatus('', `Connecting to ${ssid}...`);
            const data = await apiCall('/wifi/connect', {
                method: 'POST',
                body: JSON.stringify({ ssid: ssid, password: password || '' })
            });

            if (data) {
                if (data.success) {
                    showStatus('success', `Connected to ${ssid}`);
                    setTimeout(loadWiFiStatus, 2000);
                } else {
                    showStatus('error', `Failed to connect: ${data.error || 'Unknown error'}`);
                }
            }
        }

        // Initialize the page
        document.addEventListener('DOMContentLoaded', function() {
            loadSystemInfo();
            refreshDiscs();
            loadWiFiStatus();

            // Refresh data every 10 seconds
            setInterval(() => {
                loadWiFiStatus();
            }, 10000);
        });
    </script>
</body>
</html>
)HTML";

// URI handlers
static const httpd_uri_t uri_handlers[] = {
    { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL },
    { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL },
    { .uri = "/api/discs", .method = HTTP_GET, .handler = api_discs_handler, .user_ctx = NULL },
    { .uri = "/api/select_disc", .method = HTTP_POST, .handler = api_select_disc_handler, .user_ctx = NULL },
    { .uri = "/api/eject_disc", .method = HTTP_POST, .handler = api_eject_disc_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler, .user_ctx = NULL },
    { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = api_wifi_status_handler, .user_ctx = NULL }
};

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
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

static esp_err_t api_discs_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    if (g_server && g_server->interface_ctx) {
        // Get current disc
        char current_disc[64];
        esp_err_t disc_ret = interface_get_current_disc(g_server->interface_ctx, current_disc, sizeof(current_disc));

        if (disc_ret == ESP_OK) {
            ret = json.write("current_disc", current_disc);
        } else {
            ret = json.write("current_disc", "No disc loaded");
        }
        if (ret != ESP_OK) return ret;

        // Start discs array
        ret = json.writeKey("discs");
        if (ret != ESP_OK) return ret;

        ret = json.beginArray();
        if (ret != ESP_OK) return ret;

        // Get disc count first
        uint32_t disc_count = 0;
        disc_ret = host_comm_get_disc_count(g_server->interface_ctx->host_comm, &disc_count);

        if (disc_ret == ESP_OK && disc_count > 0) {
            // Fetch each disc one at a time and stream it
            for (uint32_t i = 0; i < disc_count && i < 64; i++) {
                disc_info_t disc_info;
                disc_ret = host_comm_get_disc_info(g_server->interface_ctx->host_comm, i, &disc_info);

                if (disc_ret == ESP_OK) {
                    ret = json.beginObject();
                    if (ret != ESP_OK) return ret;

                    ret = json.write("name", disc_info.name);
                    if (ret != ESP_OK) return ret;

                    ret = json.write("size", disc_info.size);
                    if (ret != ESP_OK) return ret;

                    ret = json.write("tracks", disc_info.tracks);
                    if (ret != ESP_OK) return ret;

                    ret = json.write("index", i);
                    if (ret != ESP_OK) return ret;

                    ret = json.endObject();
                    if (ret != ESP_OK) return ret;
                }
            }
        }

        ret = json.endArray();
        if (ret != ESP_OK) return ret;
    } else {
        ret = json.write("current_disc", "No disc loaded");
        if (ret != ESP_OK) return ret;

        ret = json.writeKey("discs");
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

static esp_err_t api_select_disc_handler(httpd_req_t *req) {
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

    cJSON *disc_index_json = cJSON_GetObjectItem(json, "disc_index");
    if (!cJSON_IsNumber(disc_index_json)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid disc_index");
        return ESP_FAIL;
    }

    uint32_t disc_index = (uint32_t)disc_index_json->valueint;

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
        esp_err_t select_ret = interface_select_disc(g_server->interface_ctx, disc_index);
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

static esp_err_t api_eject_disc_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    JsonStreamWriter json(req);
    esp_err_t ret = json.beginObject();
    if (ret != ESP_OK) return ret;

    bool success = false;
    if (g_server && g_server->interface_ctx) {
        esp_err_t eject_ret = interface_eject_disc(g_server->interface_ctx);
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
