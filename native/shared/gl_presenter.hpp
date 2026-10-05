// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "damage_tracker.hpp"
#include "er_scene.h"
#include <cstdint>
#include <span>

// Presents an opaque software framebuffer. The caller owns the GL context.
// No components, fonts, layout, platform calls or dependencies on the UI kit.
class GlPresenter {
public:
  GlPresenter() = default;
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  bool init(int width, int height);
  // Uploads the whole buffer. Afterwards the texture no longer mirrors the
  // engine framebuffer, so the next damage draw uploads everything again.
  bool draw(const std::uint32_t* argb, int surface_width, int surface_height);
  // Replays `moves` on the texture, then uploads only `damage` (damage_tracker's
  // contract); the first draw and any surface size change upload the whole buffer.
  bool draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
            int surface_width, int surface_height);
  void release();

private:
  unsigned int program_ = 0, texture_ = 0, vao_ = 0;
  // A second texture and two framebuffers, created on the first move: GL cannot blit a texture onto itself.
  unsigned int scratch_ = 0, read_fbo_ = 0, draw_fbo_ = 0;
  int width_ = 0, height_ = 0, surface_width_ = 0, surface_height_ = 0;
  bool synced_ = false;

  void upload(const std::uint32_t* argb, const ERRect& rect);
  bool move(const DamageMove& move);
  bool present(int surface_width, int surface_height);
};
