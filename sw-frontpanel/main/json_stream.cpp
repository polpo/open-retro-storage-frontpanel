#include "json_stream.h"
#include <string.h>
#include <stdio.h>

JsonStreamWriter::JsonStreamWriter(httpd_req_t* req)
    : req_(req), first_item_(true), depth_(0), in_object_(false) {
}

esp_err_t JsonStreamWriter::sendChunk(const char* chunk) {
    return httpd_resp_send_chunk(req_, chunk, strlen(chunk));
}

esp_err_t JsonStreamWriter::sendEscapedString(const char* str) {
    esp_err_t ret = sendChunk("\"");
    if (ret != ESP_OK) return ret;

    while (*str) {
        if (*str == '"' || *str == '\\') {
            char escaped[3] = {'\\', *str, '\0'};
            ret = sendChunk(escaped);
        } else if (*str == '\n') {
            ret = sendChunk("\\n");
        } else if (*str == '\r') {
            ret = sendChunk("\\r");
        } else if (*str == '\t') {
            ret = sendChunk("\\t");
        } else {
            char single[2] = {*str, '\0'};
            ret = sendChunk(single);
        }
        if (ret != ESP_OK) return ret;
        str++;
    }

    return sendChunk("\"");
}

esp_err_t JsonStreamWriter::writeSeparator() {
    if (!first_item_) {
        return sendChunk(",");
    }
    first_item_ = false;
    return ESP_OK;
}

esp_err_t JsonStreamWriter::beginObject() {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    ret = sendChunk("{");
    if (ret != ESP_OK) return ret;

    depth_++;
    in_object_ = true;
    first_item_ = true;
    return ESP_OK;
}

esp_err_t JsonStreamWriter::endObject() {
    esp_err_t ret = sendChunk("}");
    if (ret != ESP_OK) return ret;

    depth_--;
    first_item_ = false;
    in_object_ = (depth_ > 0);
    return ESP_OK;
}

esp_err_t JsonStreamWriter::beginArray() {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    ret = sendChunk("[");
    if (ret != ESP_OK) return ret;

    depth_++;
    in_object_ = false;
    first_item_ = true;
    return ESP_OK;
}

esp_err_t JsonStreamWriter::endArray() {
    esp_err_t ret = sendChunk("]");
    if (ret != ESP_OK) return ret;

    depth_--;
    first_item_ = false;
    return ESP_OK;
}

esp_err_t JsonStreamWriter::writeKey(const char* key) {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    ret = sendEscapedString(key);
    if (ret != ESP_OK) return ret;

    ret = sendChunk(":");
    if (ret != ESP_OK) return ret;

    // After writing a key, the next item (value) should not have a separator
    first_item_ = true;
    return ESP_OK;
}

esp_err_t JsonStreamWriter::writeString(const char* value) {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    return sendEscapedString(value);
}

esp_err_t JsonStreamWriter::writeNumber(uint32_t value) {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%lu", value);
    return sendChunk(buffer);
}

esp_err_t JsonStreamWriter::writeNumber(int value) {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    return sendChunk(buffer);
}

esp_err_t JsonStreamWriter::writeBool(bool value) {
    esp_err_t ret = writeSeparator();
    if (ret != ESP_OK) return ret;

    return sendChunk(value ? "true" : "false");
}

esp_err_t JsonStreamWriter::write(const char* key, const char* value) {
    esp_err_t ret = writeKey(key);
    if (ret != ESP_OK) return ret;

    first_item_ = true; // Reset for the value
    return writeString(value);
}

esp_err_t JsonStreamWriter::write(const char* key, uint32_t value) {
    esp_err_t ret = writeKey(key);
    if (ret != ESP_OK) return ret;

    first_item_ = true; // Reset for the value
    return writeNumber(value);
}

esp_err_t JsonStreamWriter::write(const char* key, int value) {
    esp_err_t ret = writeKey(key);
    if (ret != ESP_OK) return ret;

    first_item_ = true; // Reset for the value
    return writeNumber(value);
}

esp_err_t JsonStreamWriter::write(const char* key, bool value) {
    esp_err_t ret = writeKey(key);
    if (ret != ESP_OK) return ret;

    first_item_ = true; // Reset for the value
    return writeBool(value);
}

esp_err_t JsonStreamWriter::finalize() {
    return httpd_resp_send_chunk(req_, NULL, 0);
}