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
    // Every sendChunk() used to be its own HTTP chunk, so a response cost about
    // seven bytes of chunk framing per byte of JSON. Accumulate here instead and
    // send once the buffer fills.
    static const size_t kBufSize = 256;

    httpd_req_t* req_;
    bool first_item_;
    int depth_;
    bool in_object_;
    char buf_[kBufSize];
    size_t buf_len_;

    esp_err_t flush();
    esp_err_t sendChunk(const char* chunk);
    esp_err_t sendEscapedString(const char* str);
    esp_err_t writeSeparator();
};