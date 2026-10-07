// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <string>
#include <vector>
namespace archives {
// Reads the archive headers before libarchive does and returns why the archive must be refused, or an
// empty string when its declared decoder windows fit the console's memory budget.
std::string preflight(const std::vector<std::string>& sources);
}
