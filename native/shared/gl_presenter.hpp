// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstdint>
#include <vector>

// Presents an opaque software framebuffer. The caller owns the GL context.
// No components, fonts, layout, platform calls or dependencies on the UI kit.
class GlPresenter {
public:
  GlPresenter() = default;
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  bool init(int width, int height);
  bool draw(const std::uint32_t* argb, int surface_width, int surface_height);
  void release();

private:
  unsigned int program_ = 0, texture_ = 0, vao_ = 0;
  int width_ = 0, height_ = 0;
  std::vector<std::uint8_t> rgba_;
};
