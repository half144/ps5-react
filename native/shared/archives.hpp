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
// The names in a directory, from the host's own listing (a console title cannot use opendir).
bool list_directory(const char* path, void (*visit)(const char* name, void* user), void* user);
// Each extraction step, for the console's crash log; the host defines it (a no-op on the desktop).
void trace(const char* step, const char* detail = "");
// Bytes the host lets one extraction hold for buffered writes; under 1 MiB, files are written inline.
std::size_t pipeline_bytes();
}
