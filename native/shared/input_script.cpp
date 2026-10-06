// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "input_script.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr const char* kActions[] = {"up", "down", "left", "right", "confirm", "back", "quit", "shot"};
constexpr std::uint32_t kRepeatMs = 110; // The desktop host's held-key repeat interval.
} // namespace

bool InputScript::parse(const char* text, char* error, std::size_t size) {
  steps_.clear();
  index_ = 0;
  resume_ = 0;
  const std::string script(text);
  for (std::size_t start = 0; start < script.size();) {
    const std::size_t end = std::min(script.find_first_of(", \t\r\n", start), script.size());
    std::string token = script.substr(start, end - start);
    start = end + 1;
    if (token.empty()) continue;
    char* rest = nullptr;
    if (token.rfind("wait:", 0) == 0) {
      const long ms = std::strtol(token.c_str() + 5, &rest, 10);
      if (rest == token.c_str() + 5 || *rest || ms < 0) {
        std::snprintf(error, size, "bad wait '%s'", token.c_str());
        return false;
      }
      steps_.push_back({nullptr, static_cast<std::uint32_t>(ms)});
      continue;
    }
    long count = 1;
    if (const std::size_t star = token.find('*'); star != std::string::npos) {
      count = std::strtol(token.c_str() + star + 1, &rest, 10);
      if (rest == token.c_str() + star + 1 || *rest || count < 1) {
        std::snprintf(error, size, "bad repeat in '%s'", token.c_str());
        return false;
      }
      token.resize(star);
    }
    const char* action = nullptr;
    for (const char* known : kActions)
      if (token == known) action = known;
    if (!action) {
      std::snprintf(error, size, "unknown action '%s' (use up, down, left, right, confirm, back, quit, wait:MS)",
                    token.c_str());
      return false;
    }
    for (long i = 0; i < count; ++i) {
      if (i) steps_.push_back({nullptr, kRepeatMs});
      steps_.push_back({action, 0});
    }
  }
  return true;
}

const char* InputScript::next(std::uint64_t now_ms) {
  while (index_ < steps_.size() && now_ms >= resume_) {
    const Step& step = steps_[index_++];
    if (!step.action) {
      resume_ = now_ms + step.wait_ms;
      continue;
    }
    resume_ = now_ms + 1; // One action per frame.
    return step.action;
  }
  return nullptr;
}
