// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "screenshot.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include "software_backend.h"
}

bool save_screenshot(const char* path) {
  const std::uint32_t* pixels = er_software_framebuffer();
  const int source_width = er_software_fb_width(), source_height = er_software_fb_height();
  const int width = source_width / 2, height = source_height / 2, row = (width * 3 + 3) & ~3;
  if (!pixels || width <= 0 || height <= 0) return false;
  FILE* file = std::fopen(path, "wb");
  if (!file) return false;
  unsigned char header[54] = {'B', 'M'};
  const auto put = [&](int at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) header[at + i] = static_cast<unsigned char>(value >> (8 * i));
  };
  put(2, 54 + row * height); put(10, 54); put(14, 40); put(18, width); put(22, height);
  header[26] = 1; header[28] = 24;
  bool ok = std::fwrite(header, 1, sizeof header, file) == sizeof header;
  // Bottom-up rows; each output pixel averages a 2×2 block of 0xAARRGGBB words.
  std::vector<unsigned char> line(row, 0);
  for (int y = height - 1; y >= 0 && ok; --y) {
    const std::uint32_t* top = pixels + static_cast<std::size_t>(2 * y) * source_width;
    const std::uint32_t* bottom = top + source_width;
    for (int x = 0; x < width; ++x) {
      const std::uint32_t quad[] = {top[2 * x], top[2 * x + 1], bottom[2 * x], bottom[2 * x + 1]};
      for (int channel = 0; channel < 3; ++channel) {
        unsigned sum = 0;
        for (std::uint32_t p : quad) sum += (p >> (8 * channel)) & 0xFFu;
        line[x * 3 + channel] = static_cast<unsigned char>((sum + 2) / 4);
      }
    }
    ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
  }
  return std::fclose(file) == 0 && ok;
}
