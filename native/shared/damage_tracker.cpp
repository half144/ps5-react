// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "damage_tracker.hpp"

#include <algorithm>
#include <cstdint>

extern "C" {
#include "native_renderer.h"
// Engine-internal (engine/core/renderer_internal.h), exported by the pinned revision.
const EmbeddedRenderBackend* er_backend(void);
}

namespace {
EmbeddedRenderBackend inner, wrapper;
int fb_width = 0, fb_height = 0;
ERRect rects[ER_DAMAGE_RECTS_MAX];
int count = 0, last = 0;

ERRect unite(const ERRect& a, const ERRect& b) {
  const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
  return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

// Overlapping or edge-adjacent, so merging wastes no area for row-by-row blits.
bool touches(const ERRect& a, const ERRect& b) {
  return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

long long area(const ERRect& r) { return static_cast<long long>(r.w) * r.h; }

void note(int x, int y, int w, int h) {
  const int x0 = std::max(x, 0), y0 = std::max(y, 0);
  const int x1 = std::min(x + w, fb_width), y1 = std::min(y + h, fb_height);
  if (x1 <= x0 || y1 <= y0) return;
  const ERRect r = {x0, y0, x1 - x0, y1 - y0};
  if (count && touches(rects[last], r)) { rects[last] = unite(rects[last], r); return; }
  for (int i = 0; i < count; ++i)
    if (touches(rects[i], r)) { rects[last = i] = unite(rects[i], r); return; }
  if (count < ER_DAMAGE_RECTS_MAX) { rects[last = count++] = r; return; }
  // Budget exhausted: grow whichever rect wastes the least area.
  int best = 0;
  long long best_growth = -1;
  for (int i = 0; i < count; ++i) {
    const long long growth = area(unite(rects[i], r)) - area(rects[i]);
    if (best_growth < 0 || growth < best_growth) { best = i; best_growth = growth; }
  }
  rects[last = best] = unite(rects[best], r);
}

void fill(std::uint32_t argb, int x, int y, int w, int h, void* ctx) {
  inner.fill_rect(argb, x, y, w, h, ctx);
  note(x, y, w, h);
}

void copy(const void* src, int stride, int x, int y, int w, int h, void* ctx) {
  inner.copy_rect(src, stride, x, y, w, h, ctx);
  note(x, y, w, h);
}

void blend(const void* src, int stride, std::uint8_t alpha, int x, int y, int w, int h, void* ctx) {
  inner.blend_rect(src, stride, alpha, x, y, w, h, ctx);
  note(x, y, w, h);
}
} // namespace

bool damage_tracker_install(int width, int height) {
  const EmbeddedRenderBackend* active = er_backend();
  // Banded and format-aware paths write pixels this wrapper would not see.
  if (!active || !active->fill_rect || !active->copy_rect || !active->blend_rect ||
      active->band_height || active->copy_rect_fmt) return false;
  inner = wrapper = *active;
  wrapper.fill_rect = fill;
  wrapper.copy_rect = copy;
  wrapper.blend_rect = blend;
  fb_width = width; fb_height = height;
  count = last = 0;
  embedded_renderer_set_backend(&wrapper);
  return true;
}

std::span<const ERRect> damage_tracker_rects() { return {rects, static_cast<std::size_t>(count)}; }

void damage_tracker_clear() { count = last = 0; }
