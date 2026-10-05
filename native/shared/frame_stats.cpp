// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "frame_stats.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace {
// Integer tenths keep the line independent of the libc's floating-point printf.
struct Tenths { unsigned whole, tenth; };

Tenths tenths(std::uint64_t value, std::uint64_t divisor) {
  const std::uint64_t t = (value * 10 + divisor / 2) / divisor;
  return {static_cast<unsigned>(t / 10), static_cast<unsigned>(t % 10)};
}
} // namespace

const char* FrameStats::start_frame(std::int64_t now_us) {
  const char* summary = nullptr;
  if (frame_start_) {
    const auto total = static_cast<std::uint32_t>(std::max<std::int64_t>(now_us - frame_start_, 0));
    totals_[frames_ % capacity] = total;
    total_sum_ += total;
    total_max_ = std::max(total_max_, total);
    ++frames_;
  } else {
    window_start_ = now_us;
  }
  frame_start_ = mark_ = now_us;

  const std::int64_t elapsed = now_us - window_start_;
  if (frames_ && elapsed >= window_us) {
    const int count = static_cast<int>(std::min<std::uint32_t>(frames_, capacity));
    std::copy(totals_, totals_ + count, scratch_);
    std::uint32_t* p95 = scratch_ + std::min(count - 1, count * 95 / 100);
    std::nth_element(scratch_, p95, scratch_ + count);
    // Times are microseconds; dividing by 1000 * frames gives per-frame milliseconds.
    const Tenths fps = tenths(std::uint64_t{frames_} * 1000000, static_cast<std::uint64_t>(elapsed));
    const Tenths avg = tenths(total_sum_, std::uint64_t{frames_} * 1000), p = tenths(*p95, 1000),
                 max = tenths(total_max_, 1000);
    Tenths phase[phase_count];
    for (int i = 0; i < phase_count; ++i) phase[i] = tenths(phase_sum_[i], std::uint64_t{frames_} * 1000);
    std::snprintf(line_, sizeof line_,
                  "frame: fps=%u.%u total avg=%u.%ums p95=%u.%u max=%u.%u | input=%u.%u update=%u.%u present=%u.%u swap=%u.%u",
                  fps.whole, fps.tenth, avg.whole, avg.tenth, p.whole, p.tenth, max.whole, max.tenth,
                  phase[input].whole, phase[input].tenth, phase[update].whole, phase[update].tenth,
                  phase[present].whole, phase[present].tenth, phase[swap].whole, phase[swap].tenth);
    summary = line_;
    std::fill(std::begin(phase_sum_), std::end(phase_sum_), 0);
    total_sum_ = 0; total_max_ = 0; frames_ = 0;
    window_start_ = now_us;
  }
  return summary;
}

void FrameStats::lap(Phase phase, std::int64_t now_us) {
  phase_sum_[phase] += static_cast<std::uint64_t>(std::max<std::int64_t>(now_us - mark_, 0));
  mark_ = now_us;
}
