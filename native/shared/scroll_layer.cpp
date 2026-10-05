// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "scroll_layer.hpp"

#include <algorithm>
#include <cstdio>

extern "C" {
#include "software_backend.h"
}

namespace {
// 60 MiB of texture at 1920 px wide.
constexpr int kMaxRows = 8192;
// One band at a time, so the deadline is overrun by one band at most.
constexpr int kPrefetchRows = 128;
} // namespace

void scroll_layer_enable(GlPresenter& presenter) {
  const ERSoftwareLayerHost host = {
    [](int w, int h, bool* kept, void* ctx) { return static_cast<GlPresenter*>(ctx)->resize_layer(w, h, kept); },
    [](const std::uint32_t* pixels, int x, int y, int w, int h, void* ctx) {
      static_cast<GlPresenter*>(ctx)->upload_layer(pixels, x, y, w, h);
    },
    &presenter,
  };
  er_software_enable_layer(std::min(kMaxRows, presenter.max_layer_rows()), &host);
}

const ERScrollLayer* scroll_layer_frame(ERScrollLayer& placement) {
  return er_scroll_layer_get(&placement) ? &placement : nullptr;
}

const char* scroll_layer_change() {
  static ERScrollLayer last = {};
  static bool was_active = false;
  static char line[96];
  ERScrollLayer now = {};
  const bool active = er_scroll_layer_get(&now);
  if (active == was_active && (!active || (now.entries == last.entries && now.width == last.width &&
                                           now.height == last.height && now.view.y == last.view.y &&
                                           now.view.h == last.view.h)))
    return nullptr;
  was_active = active;
  last = now;
  if (!active) return "scroll layer: off";
  std::snprintf(line, sizeof line, "scroll layer #%u: %dx%d, %.1f MiB of texture, view %d+%d", now.entries, now.width,
                now.height, now.width * double((now.height + 1023) / 1024 * 1024) * 4 / (1 << 20), now.view.y,
                now.view.h);
  return line;
}

void scroll_layer_prefetch(std::int64_t (*now_us)(), std::int64_t deadline_us) {
  while (now_us() < deadline_us && er_scroll_layer_prefetch(kPrefetchRows) > 0) {}
}
