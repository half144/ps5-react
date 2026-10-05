// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "gl_presenter.hpp"
#include <cstdint>

// The engine's scroll layer (docs/SCROLL-LAYER.md) on the software backend, shared by both hosts. The layer
// lives in the presenter's texture; the backend stages one region of it at a time.

// Lets the engine paint a layer into `presenter`, up to 8192 rows (60 MiB of texture at 1920 px wide).
// Call right after er_software_backend_init(), before damage_tracker_install().
void scroll_layer_enable(GlPresenter& presenter);
// Where the last commit shows the layer, for GlPresenter::draw, or null.
const ERScrollLayer* scroll_layer_frame(ERScrollLayer& placement);
// A log line when a ScrollView entered or left the layer since the last call, else null.
const char* scroll_layer_change();
// Frame time, from its start, until which a frame may paint the layer ahead: what is left covers the
// draw and a busy frame.
constexpr std::int64_t kPrefetchUntilUs = 6000;
// Paints layer bands ahead of the viewport until `now_us()` reaches `deadline_us`, or nothing is left.
// Call right after er_commit().
void scroll_layer_prefetch(std::int64_t (*now_us)(), std::int64_t deadline_us);
