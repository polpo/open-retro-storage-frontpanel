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

#ifndef HOST_LINK_H
#define HOST_LINK_H

#include <stdbool.h>
#include "esp_err.h"
#include "transport.h"

// Helpers for the host main-board link: per-transport config assembly and,
// for auto-detecting builds, NVS persistence of the last transport that a
// live main board answered on.

// Fill *cfg with the pins/clock/address for the given transport type.
void host_link_build_config(transport_type_t type, transport_config_t *cfg);

// Load the persisted transport type. Returns false if none is stored (or the
// stored value is invalid); *type is untouched in that case.
bool host_link_load_type(transport_type_t *type);

// Persist the transport type. Skips the flash write when the stored value
// already matches.
esp_err_t host_link_save_type(transport_type_t type);

#endif // HOST_LINK_H
