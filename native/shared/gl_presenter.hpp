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
  // Uploads the whole buffer. Afterwards the GPU copy no longer mirrors the
  // engine framebuffer, so the next damage draw uploads everything again.
  bool draw(const std::uint32_t* argb, int surface_width, int surface_height);
  // Uploads what changed (damage_tracker's contract): at one-to-one size the rows of
  // `damage` and of the moved rects; otherwise `moves` are replayed on the texture and
  // then `damage` uploaded. The first draw and any surface size change upload everything.
  bool draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
            int surface_width, int surface_height);
  void release();

private:
  unsigned int program_ = 0, texture_ = 0, vao_ = 0;
  // A second texture and two framebuffers, created on the first move: GL cannot blit a texture onto itself.
  unsigned int scratch_ = 0, read_fbo_ = 0, draw_fbo_ = 0;
  int width_ = 0, height_ = 0, surface_width_ = 0, surface_height_ = 0;
  bool synced_ = false;
  // Buffer textures of whole framebuffer rows, rows [0, split_) and [split_, height_), for one-to-one
  // presentation (0 when unsupported).
  unsigned int buffer_program_ = 0, buffers_[2] = {}, buffer_textures_[2] = {};
  int split_ = 0;
  bool buffer_synced_ = false;

  bool one_to_one(int surface_width, int surface_height) const;
  void upload_changed_rows(const std::uint32_t* argb, std::span<const ERRect> damage,
                           std::span<const DamageMove> moves);
  void upload_rows(const std::uint32_t* argb, int y0, int y1);
  void release_buffer();
  void upload(const std::uint32_t* argb, const ERRect& rect);
  bool move(const DamageMove& move);
  bool present(int surface_width, int surface_height);
};
