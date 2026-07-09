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

#include "fw_version.h"
#include "sdkconfig.h"
#include <stdio.h>

// Render a 0xMMmmppPP semver (low byte = prerelease, 0xFF for a final release).
static void format_semver(char *buf, size_t buf_size, uint32_t version) {
    uint8_t major = (version >> 24) & 0xff;
    uint8_t minor = (version >> 16) & 0xff;
    uint8_t patch = (version >> 8) & 0xff;
    uint8_t pre = version & 0xff;
    if (pre == 0xff) {
        snprintf(buf, buf_size, "%u.%u.%u", major, minor, patch);
    } else {
        snprintf(buf, buf_size, "%u.%u.%u-pre%u", major, minor, patch, pre);
    }
}

void fw_version_format_panel(char *buf, size_t buf_size, uint32_t version) {
#ifdef CONFIG_PRODUCT_BLUESCSI
    // BlueSCSI panel version is packed as 0x00MMmmpp (no prerelease byte).
    uint8_t major = (version >> 16) & 0xff;
    uint8_t minor = (version >> 8) & 0xff;
    uint8_t patch = version & 0xff;
    snprintf(buf, buf_size, "%u.%u.%u", major, minor, patch);
#else
    format_semver(buf, buf_size, version);
#endif
}

void fw_version_format_mainboard(char *buf, size_t buf_size, uint32_t version) {
#ifdef CONFIG_PRODUCT_BLUESCSI
    // BlueSCSI main board uses date-based versioning (0x00YYmmdd -> YYYY.MM.DD).
    uint8_t year = (version >> 16) & 0xff;
    uint8_t month = (version >> 8) & 0xff;
    uint8_t day = version & 0xff;
    snprintf(buf, buf_size, "%d.%02d.%02d", 2000 + year, month, day);
#else
    // PicoIDE main board uses semver (0xMMmmppPP), same as the panel.
    format_semver(buf, buf_size, version);
#endif
}
