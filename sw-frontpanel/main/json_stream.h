#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

class JsonStreamWriter {
public:
    JsonStreamWriter(httpd_req_t* req);

    esp_err_t beginObject();
    esp_err_t endObject();
    esp_err_t beginArray();
    esp_err_t endArray();

    esp_err_t writeKey(const char* key);
    esp_err_t writeString(const char* value);
    esp_err_t writeNumber(uint32_t value);
    esp_err_t writeNumber(int value);
    esp_err_t writeBool(bool value);

    esp_err_t write(const char* key, const char* value);
    esp_err_t write(const char* key, uint32_t value);
    esp_err_t write(const char* key, int value);
    esp_err_t write(const char* key, bool value);

    esp_err_t finalize();

private:
    httpd_req_t* req_;
    bool first_item_;
    int depth_;
    bool in_object_;

    esp_err_t sendChunk(const char* chunk);
    esp_err_t sendEscapedString(const char* str);
    esp_err_t writeSeparator();
};