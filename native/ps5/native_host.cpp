// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Independent React proof: PS5 lifecycle/display/input, software UI and GL texture.

#include "app_config.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "core/input.hpp"
#include "damage_tracker.hpp"
#include "frame_stats.hpp"
#include "gl_presenter.hpp"
#include "host_api.hpp"
#include "host_platform.hpp"
#include "input_script.hpp"
#include "filesystem_access.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <string>
#include <pthread.h>
#include <sys/stat.h>

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
void er_register_assets(void);
extern const char proof_bundle[];
extern const unsigned long proof_bundle_length;
}

namespace {
constexpr int width = PS5_REACT_WIDTH, height = PS5_REACT_HEIGHT;
constexpr std::int64_t duration_us = static_cast<std::int64_t>(PS5_REACT_TIMEOUT) * 1000000;

void react_log(const char* line) { hui::sys::log("[REACT] %s", line); }

// An app folder may carry dev/input-script.txt (InputScript syntax) for unattended profiling; it is
// never part of a build, only added to a test deploy.
void load_input_script(InputScript& script) {
  FILE* file = std::fopen("/app0/dev/input-script.txt", "rb");
  if (!file) return;
  char text[4096] = {};
  text[std::fread(text, 1, sizeof text - 1, file)] = '\0';
  std::fclose(file);
  char error[160];
  if (script.parse(text, error, sizeof error)) hui::sys::log("[PS5-REACT] input script loaded");
  else hui::sys::log("[PS5-REACT] input script ignored: %s", error);
}

// Live commands for a running title: a test machine overwrites dev/commands.txt with lines
// "<sequence> <steps>" (InputScript syntax); about once a second the host runs the steps of every line
// whose sequence number is new. Active only when the app folder has a dev directory, which a build never
// creates. Lines present at launch are skipped.
class LiveCommands {
public:
  void start() {
    struct stat info;
    enabled_ = stat("/app0/dev", &info) == 0 && S_ISDIR(info.st_mode);
    if (enabled_) read(false);
  }

  // Appends new commands to `script` once it has finished its previous steps.
  void poll(std::int64_t now_us, InputScript& script) {
    if (!enabled_ || now_us < next_poll_us_) return;
    next_poll_us_ = now_us + 1000000;
    read(true);
    if (pending_.empty() || !script.done()) return;
    char error[160];
    if (!script.parse(pending_.c_str(), error, sizeof error)) hui::sys::log("[PS5-REACT] command ignored: %s", error);
    pending_.clear();
  }

private:
  void read(bool queue) {
    FILE* file = std::fopen("/app0/dev/commands.txt", "rb");
    if (!file) return;
    char text[4096] = {};
    text[std::fread(text, 1, sizeof text - 1, file)] = '\0';
    std::fclose(file);
    for (char* line = std::strtok(text, "\n"); line; line = std::strtok(nullptr, "\n")) {
      char* steps = nullptr;
      const unsigned long long sequence = std::strtoull(line, &steps, 10);
      if (steps == line || sequence <= last_sequence_) continue;
      steps += std::strspn(steps, " \t");
      last_sequence_ = sequence;
      if (!queue) continue;
      hui::sys::log("[PS5-REACT] command: %s", steps);
      pending_ += steps;
      pending_ += ' ';
    }
  }

  bool enabled_ = false;
  unsigned long long last_sequence_ = 0;
  std::int64_t next_poll_us_ = 0;
  std::string pending_;
};

std::uint32_t perf_clock() { return static_cast<std::uint32_t>(hui::sys::monotonic_us()); }

// Indexed like kButtonNames.
constexpr hui::Action kButtonActions[] = {
  hui::Action::up, hui::Action::down, hui::Action::left, hui::Action::right,
  hui::Action::confirm, hui::Action::back, hui::Action::north, hui::Action::west,
  hui::Action::page_prev, hui::Action::page_next, hui::Action::jump_prev, hui::Action::jump_next,
  hui::Action::l3, hui::Action::r3, hui::Action::menu, hui::Action::touch,
};
static_assert(std::size(kButtonActions) == std::size(kButtonNames));

GamepadState gamepad_state(const hui::InputFrame& input) {
  GamepadState state;
  state.connected = input.connected;
  state.left_x = input.stick_x; state.left_y = input.stick_y;
  state.right_x = input.stick2_x; state.right_y = input.stick2_y;
  state.l2 = input.trigger_l; state.r2 = input.trigger_r;
  for (std::size_t i = 0; i < std::size(kButtonActions); ++i)
    if (input.is_held(kButtonActions[i])) state.buttons |= 1u << i;
  return state;
}

const char* nav_action(hui::Direction direction) {
  switch (direction) {
    case hui::Direction::up: return "up";
    case hui::Direction::down: return "down";
    case hui::Direction::left: return "left";
    case hui::Direction::right: return "right";
    default: return nullptr;
  }
}

bool dispatch(const char* action) {
  er_perf_phase_begin(ER_PERF_PHASE_JS);
  JSContext* ctx = er_runtime_context();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, global, "__ps5ReactDispatch");
  JSValue value = JS_NewString(ctx, action);
  JSValue result = JS_Call(ctx, fn, global, 1, &value);
  const bool ok = !JS_IsException(result);
  if (!ok) {
    JSValue error = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, error);
    hui::sys::log("[PS5-REACT] input exception: %s", message ? message : "unknown");
    if (message) JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
  }
  JS_FreeValue(ctx, result); JS_FreeValue(ctx, value);
  JS_FreeValue(ctx, fn); JS_FreeValue(ctx, global);
  er_perf_phase_end(ER_PERF_PHASE_JS);
  return ok;
}

// Owned resources are released while the context is valid; closing the title
// happens after this scope, through the platform's lifecycle API.
bool run_proof() {
  hui::ps5::Display display;
  hui::ps5::Pad pad;
  GlPresenter presenter;
  hui::InputTracker tracker;
  hui::PadSample samples[64];
  FrameStats stats;
  bool runtime = false, software = false;
  bool ok = display.open(PS5_REACT_SURFACE_WIDTH, PS5_REACT_SURFACE_HEIGHT);
  hui::sys::log("[PS5-REACT] display=%d", ok);
  if (ok) {
    ok = presenter.init(width, height);
    hui::sys::log("[PS5-REACT] presenter=%d", ok);
  }
  if (ok) {
    software = er_software_backend_init(width, height);
    ok = software && damage_tracker_install(width, height);
    hui::sys::log("[PS5-REACT] software framebuffer=%d %dx%d", ok, width, height);
  }
  // The pad opens before the bundle runs: it also initializes the user service
  // that the users, notification and browser calls need.
  if (ok) {
    hui::sys::log("[PS5-REACT] pad=%d", pad.open());
    host_platform_set_pad(&pad);
  }
  if (ok) {
    ErRuntimeConfig config = {};
    config.screen_width = width; config.screen_height = height;
    config.screen_scale = 2;
    config.log = react_log;
    config.max_stack_size = 1024 * 1024;
    config.memory_limit = 32 * 1024 * 1024;
    config.install_host_globals = ps5_react_install_host_api;
    runtime = er_runtime_init(&config);
    ok = runtime;
    hui::sys::log("[PS5-REACT] runtime=%d", ok);
  }
  if (ok) {
    er_register_assets();
    ok = er_runtime_load_source(proof_bundle, proof_bundle_length, "app.jsx.bundle");
    hui::sys::log("[PS5-REACT] bundle=%d gc_accounting=%d", ok, er_runtime_gc_accounting_ok());
  }
  if (ok) {
    // Input failure is logged; timeout still allows the display-only proof to end.
    hui::sys::log("[PS5-REACT] Options closes; timeout=%ds after first frame", PS5_REACT_TIMEOUT);
    std::int64_t first_present = 0, previous = hui::sys::monotonic_us();
    std::uint64_t frames = 0;
    InputScript script;
    load_input_script(script);
    LiveCommands commands;
    commands.start();
    er_perf_set_clock(perf_clock);
    while (ok) {
      const std::int64_t now = hui::sys::monotonic_us();
      if (now <= 0 || (duration_us > 0 && first_present && now - first_present >= duration_us)) break;
      if (const char* line = stats.start_frame(now)) hui::sys::log("[PS5-REACT] %s", line);
      const auto count = pad.read(samples);
      const auto input = tracker.update(std::span<const hui::PadSample>(samples, count), now);
      if (input.is_pressed(hui::Action::menu)) break;
      pad.tick(static_cast<float>(now - previous) / 1000000.0f);
      ps5_react_set_gamepad(gamepad_state(input));
      if (const char* direction = nav_action(input.nav)) ok = dispatch(direction);
      if (ok && input.is_pressed(hui::Action::confirm)) ok = dispatch("confirm");
      if (ok && input.is_pressed(hui::Action::back)) ok = dispatch("back");
      commands.poll(now, script);
      if (const char* action = ok ? script.next(static_cast<std::uint64_t>(now / 1000)) : nullptr) {
        hui::sys::log("[PS5-REACT] script: %s", action);
        if (!std::strcmp(action, "quit")) break;
        ok = dispatch(action);
      }
      if (!ok) break;
      stats.lap(FrameStats::input, hui::sys::monotonic_us());
      er_perf_phase_begin(ER_PERF_PHASE_JS);
      er_runtime_pump();
      er_perf_phase_end(ER_PERF_PHASE_JS);
      er_commit();
      if (*er_runtime_last_error()) { ok = false; break; }
      stats.lap(FrameStats::update, hui::sys::monotonic_us());
      er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
      ok = presenter.draw(er_software_framebuffer(), damage_tracker_rects(), damage_tracker_moves(), display.width(),
                          display.height());
      er_perf_phase_end(ER_PERF_PHASE_PRESENT);
      damage_tracker_clear();
      stats.lap(FrameStats::present, hui::sys::monotonic_us());
      ok = ok && display.swap();
      stats.lap(FrameStats::swap, hui::sys::monotonic_us());
      if (!ok) break;
      if (!first_present) {
        first_present = hui::sys::monotonic_us();
        hui::sys::hide_splash_screen();
        hui::sys::log("[PS5-REACT] first frame presented");
      }
      embedded_renderer_tick(static_cast<std::uint32_t>(std::clamp<std::int64_t>((now-previous)/1000, 0, 50)));
      previous = now;
      ++frames;
      if (const char* line = stats.end_frame(33000)) hui::sys::log("[PS5-REACT] %s", line);
      if (ps5_react_exit_requested()) break;
    }
    hui::sys::log("[PS5-REACT] loop ended ok=%d frames=%llu", ok, static_cast<unsigned long long>(frames));
  }
  if (!ok && runtime) hui::sys::log("[PS5-REACT] error=%s", er_runtime_last_error());
  if (runtime) er_runtime_shutdown();
  if (software) er_software_backend_destroy();
  host_platform_set_pad(nullptr);
  pad.close();
  presenter.release();
  display.close();
  return ok;
}

void* render_thread(void*) {
  hui::sys::log("[PS5-REACT] render thread started, 8 MiB stack");
  const bool ok = run_proof();
  hui::sys::log("[PS5-REACT] cleanup complete, result=%d; requesting title closure", ok);
  hui::sys::quit();
  return nullptr;
}
} // namespace

int main() {
  hui::sys::log("[PS5-REACT] %s (%s)", PS5_REACT_NAME, PS5_REACT_TITLE);
  initialize_filesystem_access();
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (!result) {
    result = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
    pthread_t thread;
    if (!result) result = pthread_create(&thread, &attributes, render_thread, nullptr);
    pthread_attr_destroy(&attributes);
  }
  if (result) {
    hui::sys::log("[PS5-REACT] thread creation failed=%d", result);
    hui::sys::quit();
  }
  // Native titles must not return to CRT exit(); the render thread owns closure.
  hui::sys::park();
  return 0;
}
