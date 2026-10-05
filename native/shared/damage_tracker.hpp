// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "er_scene.h"
#include <span>

// Records every rect the engine writes into the software framebuffer, so the
// presenter can upload only what changed since the last frame.
//
// er_get_dirty_rects() is not enough: it reports only the last commit that
// painted, while one host frame can run several (a controller dispatch commits
// outside the pump's batch, and layout handlers can commit inside er_commit).
// Wrapping the backend's blits observes all of them.

// Call right after er_software_backend_init(), before any asset or runtime setup:
// re-registering the backend resets the engine's font and image registries.
bool damage_tracker_install(int width, int height);
// Rects painted since the last clear; they may overlap.
std::span<const ERRect> damage_tracker_rects();
void damage_tracker_clear();
