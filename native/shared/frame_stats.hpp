// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstdint>

// Wall-clock frame split, summarised once per window. The host supplies a
// monotonic microsecond clock; nothing here allocates or calls the platform.
class FrameStats {
public:
  enum Phase { input, update, present, swap, phase_count };

  // Starts a frame. Returns the summary line when the previous frame closed a
  // window, otherwise nullptr. The line stays valid until the next call.
  const char* start_frame(std::int64_t now_us);
  // Attributes the time since the previous start_frame()/lap() to `phase`.
  void lap(Phase phase, std::int64_t now_us);

private:
  static constexpr std::int64_t window_us = 2000000;
  // Holds a full window up to 512 Hz; beyond that p95 covers the latest frames.
  static constexpr int capacity = 1024;

  std::uint32_t totals_[capacity] = {}, scratch_[capacity] = {};
  std::uint64_t phase_sum_[phase_count] = {}, total_sum_ = 0;
  std::uint32_t total_max_ = 0, frames_ = 0;
  std::int64_t window_start_ = 0, frame_start_ = 0, mark_ = 0;
  char line_[192] = {};
};
