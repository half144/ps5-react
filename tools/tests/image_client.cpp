// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Drives native/shared/image_loader.cpp for tools/test_images.py; one scenario per run.
#include "image_loader.hpp"
#include "network.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::vector<std::uint32_t> evicted;

std::vector<images::Result> wait(std::size_t count, int timeout_ms) {
  std::vector<images::Result> out;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (out.size() < count && std::chrono::steady_clock::now() < until) {
    for (auto& r : images::poll([](std::uint32_t id) { evicted.push_back(id); })) out.push_back(r);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return out;
}

void print(const images::Result& r) {
  std::printf("{\"id\":%u,\"ready\":%s,\"failed\":%s,\"width\":%d,\"height\":%d,\"opaque\":%s,\"error\":\"%s\"}\n",
              r.id, r.ready ? "true" : "false", r.failed ? "true" : "false", r.width, r.height,
              r.opaque ? "true" : "false", r.error.c_str());
}

std::uint32_t load(const std::string& url, int w, int h, int fit, images::Result& result) {
  std::string error;
  const auto id = images::load(url, w, h, static_cast<images::Fit>(fit), false, result, error);
  if (!id) { std::cerr << error << '\n'; std::exit(4); }
  return id;
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || !network::start() || !images::start(std::getenv("IMAGE_CACHE") ? std::getenv("IMAGE_CACHE") : "")) return 3;
  const std::string mode = argv[1], url = argv[2];
  images::Result now;
  if (mode == "load" && argc >= 7) {
    load(url, std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]), now);
    const auto results = wait(1, 10000);
    if (results.empty()) return 5;
    print(results[0]);
    if (results[0].ready) {
      FILE* file = std::fopen(argv[6], "wb");
      std::fwrite(results[0].pixels, 4, static_cast<std::size_t>(results[0].width) * results[0].height, file);
      std::fclose(file);
    }
  } else if (mode == "shared") {
    // Two references to one box share an entry, another box shares the fetch, and a finished
    // entry reports at once.
    images::Result second, third, other;
    const auto a = load(url, 64, 64, 0, now), b = load(url, 64, 64, 0, second);
    load(url, 32, 32, 0, other);
    if (a != b || wait(2, 10000).size() != 2) return 6;
    const auto c = load(url, 64, 64, 0, third);
    std::printf("{\"same\":%s,\"immediate\":%s}\n", c == a ? "true" : "false", third.ready ? "true" : "false");
  } else if (mode == "cancel") {
    // A release before completion cancels the transfer and never reports.
    const auto id = load(url, 64, 64, 0, now);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    images::release(id);
    std::printf("{\"reported\":%zu}\n", wait(1, 1500).size());
  } else if (mode == "evict") {
    // Unused images over budget are evicted oldest first; images in use never are.
    const int count = std::atoi(argv[3]);
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < count; ++i) ids.push_back(load(url + "?n=" + std::to_string(i), 1024, 1024, 2, now));
    const auto results = wait(static_cast<std::size_t>(count), 30000);
    for (std::size_t i = 1; i < ids.size(); ++i) images::release(ids[i]);
    wait(1, 50);
    std::printf("{\"loaded\":%zu,\"evicted\":%zu,\"first\":%u,\"kept\":%s}\n", results.size(), evicted.size(),
                evicted.empty() ? 0 : evicted.front(),
                std::find(evicted.begin(), evicted.end(), ids[0]) == evicted.end() ? "true" : "false");
  } else {
    return 2;
  }
  images::stop([](std::uint32_t) {});
  network::stop();
  return 0;
}
