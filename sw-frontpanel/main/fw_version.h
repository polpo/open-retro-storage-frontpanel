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

#ifndef FW_VERSION_H
#define FW_VERSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Format the front panel firmware version (always semver) into buf. The packed
// encoding is product-specific (the main board supplies the available version
// over the panel protocol): 0x00MMmmpp on BlueSCSI, 0xMMmmppPP (low byte =
// prerelease) on PicoIDE. Both render as "major.minor.patch[-preN]".
void fw_version_format_panel(char *buf, size_t buf_size, uint32_t version);

// Format the main board firmware version into buf. BlueSCSI uses date-based
// versioning (0x00YYmmdd -> "YYYY.MM.DD"); PicoIDE uses semver (0xMMmmppPP).
void fw_version_format_mainboard(char *buf, size_t buf_size, uint32_t version);

#ifdef __cplusplus
}
#endif

#endif // FW_VERSION_H
