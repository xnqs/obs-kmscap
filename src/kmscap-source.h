/*
 * obs-kmscap - Zero-copy KMS/DRM screen capture plugin for OBS Studio
 * Copyright (C) 2026 Robert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>
 */

#pragma once

#include <obs-module.h>

/**
 * OBS source info struct for the KMS screen capture source.
 * Register this with obs_register_source() in obs_module_load().
 */
extern struct obs_source_info kmscap_source_info;
