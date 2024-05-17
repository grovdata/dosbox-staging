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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "cpu_settings.h"

#include <deque>
#include <regex>

#include "checks.h"
#include "string_utils.h"

CHECK_NARROWING();

// All valid cycles setting variations supported by original DOSBox:
//
//   12000
//   fixed 12000
//
//   max
//   max limit 50000
//   max 90%
//   max 90% limit 50000
//
//   auto limit 50000           (implicit "3000" for real mode & "max 100%")
//   auto 90%                   (implicit "3000" for real mode)
//   auto 90% limit 50000       (implicit "3000" for real mode]
//
//   auto max                   (implicit "3000" for real mode)
//   auto max limit 50000       (implicit "3000" for real mode)
//   auto max 90%               (implicit "3000" for real mode)
//   auto max 90% limit 50000   (implicit "3000" for real mode)
//
//   auto 12000                 (implicit "max 100%"
//   auto 12000 limit 50000     (implicit "max 100%")
//   auto 12000 90%
//   auto 12000 90% limit 50000
//
//   auto 12000 max
//   auto 12000 max limit 50000
//   auto 12000 max 90%
//   auto 12000 max 90% limit 50000
//
/*std::optional<CpuCyclesConfig> CPU_ParseCyclesSetting(const std::string& pref)
{
	const auto parts = split(pref);
	std::deque<std::string> s = {parts.begin(), parts.end()}; 
	
	if (s.size() == 1) {
		if (s[0] == "max") {
			//
		} else if (const auto i = parse_int(s[0]); i) {
			//
		}

	} else {
		if (s[0] == "max") {
			s.pop_front();

			if (s.empty()) {

			} else if (s.size() == 1) {
				if (const auto p = parse_percentage_with_percent_sign(s[0]); p) {
					//
				}

			} else if (s.size() == 2) {
				const auto i = parse_int(s[1]);
				if (s[0] == "limit" && i) {
					//
				}

			} else if (s.size() == 3) {
				const auto p = parse_percentage_with_percent_sign(s[0]);
				const auto i = parse_int(s[2]);
				if (p && s[1] == "limit" && i) {
					//
				}
			}

		} else if (s[0] == "auto") {
			s.pop_front();

		}
	}

	CpuCyclesConfig config = {};


	return {};
}
*/

const std::string Whitespace         = R"(\s+)";
const std::string OptionalWhitespace = R"(\s*)";
const std::string String             = R"([^\s]*)";
const std::string Integer            = R"([+-]?\d+)";
const std::string Decimal            = R"([+-]?\d+(?:\.\d)?\d*)";

std::vector<std::regex> convert_pattern_to_regex(const std::string& pattern)
{
	std::stringstream regex_str;
	auto num_groups = 0;

	const auto terms = split(pattern);
	auto it = terms.begin();

	while (it != terms.end()) {
		const auto p = *it;

		if (p.starts_with('[') && p.ends_with(']')) {
			// Optional keyword
			const auto keyword = std::string(p.substr(1, p.size() - 2));
			regex_str << "(?:" << keyword << OptionalWhitespace << ")?";

		} else {
			if (p == "%s") {
				// String
				regex_str << "(" << String << ")";
				++num_groups;

			} else if (p == "%i") {
				// Integer
				regex_str << "(" << Integer << ")";
				++num_groups;

			} else if (p == "%i%") {
				// Integer percentage
				regex_str << "(" << Integer << "%"
				          << ")";
				++num_groups;

			} else if (p == "%d") {
				// Decimal
				regex_str << "(" << Decimal << ")";
				++num_groups;

			} else if (p == "%d%") {
				// Decimal percentage
				regex_str << "(" << Decimal << "%"
				          << ")";
				++num_groups;

			} else if (p == "...") {
				// Tail string
				regex_str << "(.*)";
				++num_groups;

			} else {
				// Mandatory keyword
				regex_str << "(?:" << p << ")";
			}

			const auto is_last = std::next(it) != terms.end();
			if (is_last) {
				regex_str << Whitespace;
			}
		}

		++it;
	}

//	printf("\n\n%s\n\n", regex_str.str().c_str());

	// Return a case-insensitive regular expression
	const auto regex = std::regex(regex_str.str(), std::regex_constants::icase);
	return {regex, num_groups};
}

std::optional<std::vector<std::string>> match_pattern(const std::string& str,
                                                      const std::string& pattern)
{
	printf("\n*** str: %s, match_pattern: %s\n", str.c_str(),pattern.c_str());

	auto [regex, num_groups] = convert_pattern_to_regex(pattern);

	std::smatch match = {};
	std::regex_match(str, match, regex);

	printf("       num_groups: %d, match_size: %d\n", num_groups, match.size());

	if (num_groups != match.size() - 1) {
		return {};
	}

	std::vector<std::string> results = {};

	for (auto i = 1; i < match.size(); ++i) {
		results.emplace_back(match[i].str());
	}
	return results;
}

static bool parse_percentage_cycles(const std::string& s, CpuCyclesConfig& config)
{
	if (s == "") {
		// [max]
		config.percentage = 100;
		return true;

	} else if (const auto maybe_match = match_pattern(s, "limit %i")) {
		// [max] limit 50000
		const auto match = *maybe_match;

		const auto limit = *parse_int(match[0]);
		config.limit     = limit;
		return true;

	} else if (const auto maybe_match = match_pattern(s, "%i%")) {
		// [max] 90%
		const auto match = *maybe_match;

		const auto percentage = *parse_int(match[0]);
		config.percentage     = percentage;
		return true;

	} else if (const auto maybe_match = match_pattern(s, "%i% limit %i")) {
		// [max] 90% limit 50000
		const auto match = *maybe_match;

		const auto percentage = *parse_int(match[0]);
		config.percentage     = percentage;

		const auto limit = *parse_int(match[1]);
		config.limit     = limit;
		return true;
	}

	return false;
}

std::optional<CpuCyclesConfig> CPU_ParseCyclesSetting(const std::string& pref)
{
	CpuCyclesConfig config = {};

	if (const auto maybe_match = match_pattern(pref, "%i")) {

	} else if (const auto maybe_match = match_pattern(pref, "[fixed] %i")) {
		// 12000
		// fixed 12000
		const auto match = *maybe_match;

		const auto fixed = *parse_int(match[0]);
		config.fixed     = fixed;

	} else if (const auto maybe_match = match_pattern(pref, "max ...")) {
		// max
		// max limit 50000
		// max 90%
		// max 90% limit 50000
		const auto match = *maybe_match;

		const auto tail = match[0];
		if (!parse_percentage_cycles(tail, config)) {
			config = {};
		}

	} else if (const auto maybe_match = match_pattern(pref, "auto max ...")) {
		// auto max                   (implicit "3000" for real mode)
		// auto max limit 50000       (implicit "3000" for real mode)
		// auto max 90%               (implicit "3000" for real mode)
		// auto max 90% limit 50000   (implicit "3000" for real mode)
		const auto match = *maybe_match;

		const auto tail = match[0];
		if (!parse_percentage_cycles(tail, config)) {
			config = {};
		}

	} else if (const auto maybe_match = match_pattern(pref, "auto %i ...")) {
		// auto 12000                 (implicit "max 100%"
		// auto 12000 limit 50000     (implicit "max 100%")
		// auto 12000 90%
		// auto 12000 90% limit 50000
		//
		const auto match = *maybe_match;

		const auto fixed = *parse_int(match[0]);
		config.fixed     = fixed;

		const auto tail = match[1];
		if (!parse_percentage_cycles(tail, config)) {
			config = {};
		}

	} else if (const auto maybe_match = match_pattern(pref, "auto %i max ...")) {
		// auto 12000 max
		// auto 12000 max limit 50000
		// auto 12000 max 90%
		// auto 12000 max 90% limit 50000
		//
		// auto 12000                 (implicit "max 100%"
		// auto 12000 limit 50000     (implicit "max 100%")
		// auto 12000 90%
		// auto 12000 90% limit 50000
		//
		const auto match = *maybe_match;

		const auto fixed = *parse_int(match[0]);
		config.fixed     = fixed;

		const auto tail = match[1];
		if (!parse_percentage_cycles(tail, config)) {
			config = {};
		}
	}

	return config;
}

// new:
//
//   12000
//   throttled 50000               (max limit 50000)
//   max
//
//   real 12000 protected max               (auto 12000 max)
//   real 12000 protected 50000 throttled   (auto 12000 max limit 50000)
