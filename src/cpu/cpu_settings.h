/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2024-2024  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_CPU_SETTINGS_H
#define DOSBOX_CPU_SETTINGS_H

#include <optional>
#include <string>

#include "cpu.h"

struct CpuCyclesConfig {
	std::optional<int> fixed      = {};
	std::optional<int> percentage = {};
	std::optional<int> limit      = {};
};

constexpr CpuCyclesConfig DefaultConfig = {.fixed = CpuCyclesRealModeDefault,
                                           .percentage = 100,
                                           .limit      = 60000};

std::optional<CpuCyclesConfig> CPU_ParseCyclesSetting(const std::string& pref);

#endif // DOSBOX_CPU_SETTINGS_H
