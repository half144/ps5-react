// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "quickjs.h"
}

// The bridge's default JS-heap allocator, counting the bytes it holds. Pass it as
// ErRuntimeConfig.malloc_functions; everything else runs on the render thread.
const JSMallocFunctions* js_heap_functions();

// QuickJS collects cycles when its heap grows past a threshold, in whichever
// allocation crosses it: a full mark-and-sweep of every live object (tens of
// milliseconds on the PS5 for a store's catalog) inside the frame that handles a
// key press. This collects in idle frames instead (half a second after the last
// input), once a third of the room before that threshold is garbage, so a burst
// of input starts with at least two thirds of it.
// It never raises QuickJS's threshold: a collection is only ever brought forward.
class GcScheduler {
public:
  // `clock` is the host's monotonic microsecond clock, to time collections.
  explicit GcScheduler(std::int64_t (*clock)()) : clock_(clock) {}
  // Call when the host dispatches an input action.
  void input(std::int64_t now_us) { last_input_us_ = now_us; }
  // Call once per frame, after it is presented. Returns a log line when a
  // collection ran during the frame or here, otherwise nullptr.
  const char* frame(JSRuntime* runtime, std::int64_t now_us);

private:
  std::int64_t (*clock_)();
  std::int64_t last_input_us_ = 0;
  std::size_t threshold_ = 0, live_after_gc_ = 0;
  char line_[160] = {};
};
