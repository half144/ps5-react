// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Remote images: two workers fetch (libcurl) and decode (stb_image) off the render thread and
// resample to the size the image is drawn at. The render thread only submits, releases and polls.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace images {
// How a decoded image is fitted to the requested box, as the engine's resizeMode draws it.
enum class Fit : std::uint8_t { cover, contain, stretch, none };

// Decoded pixels that stay cached while unused, least recently used evicted first. Images in use
// are never evicted, so a screen that shows more than this exceeds it.
constexpr std::size_t kCacheBytes = 32 * 1024 * 1024;
constexpr std::size_t kCacheEntries = 128;
constexpr std::size_t kMaxEncodedBytes = 8 * 1024 * 1024;
// Encoded bytes kept by URL, so the same image drawn at another size decodes without a fetch.
constexpr std::size_t kEncodedCacheBytes = 8 * 1024 * 1024;
// Encoded responses kept on disk between launches; entries older than a week are fetched again.
constexpr std::size_t kDiskCacheBytes = 64 * 1024 * 1024;
// One image decodes at a time, holding up to about 8 bytes per source pixel (PNG) while it does.
constexpr std::uint64_t kMaxSourcePixels = 5000000;

struct Result {
  std::uint32_t id = 0;
  bool ready = false, failed = false;
  int width = 0, height = 0;
  bool opaque = true;
  const std::uint32_t* pixels = nullptr; // premultiplied ARGB8888; valid until evicted
  std::string error;
};

// start() requires a started network::start(); `cache_directory` (created when missing, empty to
// disable) holds the disk cache. stop() calls `evict` for every image handed out by poll(), then
// joins both workers and frees every image.
bool start(const std::string& cache_directory);
void stop(void (*evict)(std::uint32_t id));
// Render thread. Returns the id (0 with `error` on invalid input); each load takes one reference,
// shared by every load of the same url, box and fit. `result` reports a finished entry at once.
std::uint32_t load(const std::string& url, int width, int height, Fit fit, Result& result, std::string& error);
// Drops one reference. Unfinished work without references is cancelled.
void release(std::uint32_t id);
// Finished loads since the last poll. Evicts unused images over budget first, calling `evict`
// before their pixels are freed.
std::vector<Result> poll(void (*evict)(std::uint32_t id));
// Turns a ready image the caller could not use into a failure, freeing its pixels.
void discard(std::uint32_t id, const std::string& reason);
} // namespace images
