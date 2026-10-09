// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace archives {
inline constexpr std::uint64_t max_entries = 100000;
inline constexpr std::size_t max_metadata_bytes = 16 * 1024 * 1024;
struct Request { std::vector<std::string> sources; std::string destination, password; std::uint64_t max_bytes = 1024ULL*1024*1024*1024; };
struct Snapshot { std::uint32_t id = 0; std::string state = "queued", error, destination; std::uint64_t written = 0, entries = 0; std::vector<std::string> artifacts; };
std::uint32_t enqueue(Request request, std::string& error);
void cancel(std::uint32_t id);
std::vector<Snapshot> poll();
void stop();
}
