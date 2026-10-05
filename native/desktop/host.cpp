// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "app_config.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
void er_register_assets(void);
}
#include "damage_tracker.hpp"
#include "frame_stats.hpp"
#include "gl_presenter.hpp"
#include "host_api.hpp"
#include "host_platform.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace {
// Render at twice the logical resolution so rounded edges remain smooth on Retina.
constexpr int width = PS5_REACT_WIDTH, height = PS5_REACT_HEIGHT;

std::int64_t now_us() {
  static const Uint64 frequency = SDL_GetPerformanceFrequency();
  const Uint64 ticks = SDL_GetPerformanceCounter();
  return static_cast<std::int64_t>(ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency);
}

float stick(SDL_GameController* controller, SDL_GameControllerAxis axis) {
  constexpr float deadzone = 0.16f;
  const float value = std::clamp(SDL_GameControllerGetAxis(controller, axis) / 32767.0f, -1.0f, 1.0f);
  if (std::abs(value) < deadzone) return 0;
  return std::copysign((std::abs(value) - deadzone) / (1 - deadzone), value);
}

GamepadState read_gamepad(SDL_GameController* controller) {
  GamepadState state;
  // Indexes follow kButtonNames; l2/r2 come from the triggers below.
  constexpr int buttons[] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -1,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_TOUCHPAD,
  };
  if (controller && SDL_GameControllerGetAttached(controller)) {
    state.connected = true;
    state.left_x = stick(controller, SDL_CONTROLLER_AXIS_LEFTX);
    state.left_y = stick(controller, SDL_CONTROLLER_AXIS_LEFTY);
    state.right_x = stick(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    state.right_y = stick(controller, SDL_CONTROLLER_AXIS_RIGHTY);
    state.l2 = std::max(0.0f, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f);
    state.r2 = std::max(0.0f, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f);
    for (std::uint32_t i = 0; i < std::size(buttons); ++i)
      if (buttons[i] >= 0 && SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(buttons[i])))
        state.buttons |= 1u << i;
    if (state.l2 > 0.5f) state.buttons |= 1u << 10;
    if (state.r2 > 0.5f) state.buttons |= 1u << 11;
  }
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  constexpr SDL_Scancode keyboard[] = {
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
  };
  for (std::uint32_t i = 0; i < std::size(keyboard); ++i)
    if (keys[keyboard[i]]) state.buttons |= 1u << i;
  return state;
}

struct Host {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  SDL_GameController* controller = nullptr;
  GlPresenter presenter;
  FrameStats stats;
  bool log_frames = true, running = true, runtime_started = false, backend_started = false;
  Uint32 previous_tick = 0, next_repeat = 0;
  const char* held_action = nullptr;
  SDL_Scancode held_key = SDL_SCANCODE_UNKNOWN;
  SDL_GameControllerButton held_button = SDL_CONTROLLER_BUTTON_INVALID;

  ~Host() {
    if (runtime_started) er_runtime_shutdown();
    if (backend_started) er_software_backend_destroy();
    presenter.release(); // GL objects must be deleted before their context.
    desktop_set_controller(nullptr);
    if (controller) SDL_GameControllerClose(controller);
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
  }

  bool start() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)) return false;
    window = SDL_CreateWindow(PS5_REACT_NAME,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width/2, height/2,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window || !(context = SDL_GL_CreateContext(window))) return false;
    SDL_GL_SetSwapInterval(1);
    std::printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
      if (SDL_IsGameController(i) && (controller = SDL_GameControllerOpen(i))) break;
    std::printf("Physical controller: %s\n", controller ? SDL_GameControllerName(controller) : "not connected");
    desktop_set_controller(controller);
    previous_tick = SDL_GetTicks();
    return presenter.init(width, height);
  }

  bool boot(const char* path) {
    backend_started = er_software_backend_init(width, height);
    if (!backend_started || !damage_tracker_install(width, height)) return false;
    ErRuntimeConfig cfg = {};
    cfg.screen_width = width; cfg.screen_height = height; cfg.screen_scale = 2;
    cfg.memory_limit = 32 * 1024 * 1024;
    cfg.max_stack_size = 1024 * 1024;
    cfg.install_host_globals = ps5_react_install_host_api;
    runtime_started = er_runtime_init(&cfg);
    if (!runtime_started) return false;
    er_register_assets();
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    ok = size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    const std::size_t length = ok ? static_cast<std::size_t>(size) : 0;
    // QuickJS's source parser requires a trailing NUL beyond the supplied length.
    std::vector<char> source(length + 1, '\0');
    if (ok) ok = std::fread(source.data(), 1, length, file) == length;
    std::fclose(file);
    return ok && er_runtime_load_source(source.data(), length, path);
  }

  const char* hold(const char* action, SDL_Scancode key,
                   SDL_GameControllerButton button = SDL_CONTROLLER_BUTTON_INVALID) {
    held_action = action; held_key = key; held_button = button;
    next_repeat = SDL_GetTicks() + 350;
    return action;
  }

  bool dispatch(const char* action) {
    JSContext* ctx = er_runtime_context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__ps5ReactDispatch");
    JSValue arg = JS_NewString(ctx, action);
    JSValue result = JS_Call(ctx, fn, global, 1, &arg);
    const bool ok = !JS_IsException(result);
    if (!ok) {
      JSValue error = JS_GetException(ctx);
      const char* message = JS_ToCString(ctx, error);
      std::fprintf(stderr, "Input failed: %s\n", message ? message : "JS exception");
      if (message) JS_FreeCString(ctx, message);
      JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, fn); JS_FreeValue(ctx, global);
    return ok;
  }

  bool frame(bool swap = true) {
    if (const char* line = stats.start_frame(now_us()); line && log_frames) {
      std::printf("[PS5-REACT] %s\n", line);
      std::fflush(stdout); // The dev watcher reads a pipe, where stdout is fully buffered.
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      const char* action = nullptr;
      if (event.type == SDL_QUIT) running = false;
      // The host repeats held directions itself, so OS key repeats are ignored.
      if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const SDL_Scancode key = event.key.keysym.scancode;
        switch (event.key.keysym.sym) {
          case SDLK_UP: action = hold("up", key); break;
          case SDLK_DOWN: action = hold("down", key); break;
          case SDLK_LEFT: action = hold("left", key); break;
          case SDLK_RIGHT: action = hold("right", key); break;
          case SDLK_RETURN: case SDLK_SPACE: action = "confirm"; break;
          case SDLK_BACKSPACE: action = "back"; break;
          case SDLK_ESCAPE: running = false; break;
        }
      } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        const auto button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
        switch (button) {
          case SDL_CONTROLLER_BUTTON_DPAD_UP: action = hold("up", SDL_SCANCODE_UNKNOWN, button); break;
          case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = hold("down", SDL_SCANCODE_UNKNOWN, button); break;
          case SDL_CONTROLLER_BUTTON_DPAD_LEFT: action = hold("left", SDL_SCANCODE_UNKNOWN, button); break;
          case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: action = hold("right", SDL_SCANCODE_UNKNOWN, button); break;
          case SDL_CONTROLLER_BUTTON_A: action = "confirm"; break;
          case SDL_CONTROLLER_BUTTON_B: action = "back"; break;
          default: break;
        }
      }
      if (action && !dispatch(action)) return false;
    }
    if (held_action) {
      // Polled state, not key-up events: synthetic self-test presses never stay held.
      const bool down = held_key != SDL_SCANCODE_UNKNOWN ? SDL_GetKeyboardState(nullptr)[held_key]
        : controller && SDL_GameControllerGetButton(controller, held_button);
      const Uint32 ticks = SDL_GetTicks();
      if (!down) held_action = nullptr;
      else if (SDL_TICKS_PASSED(ticks, next_repeat)) {
        next_repeat = ticks + 110;
        if (!dispatch(held_action)) return false;
      }
    }
    if (!running) return true;
    ps5_react_set_gamepad(read_gamepad(controller));
    stats.lap(FrameStats::input, now_us());
    er_runtime_pump(); er_commit();
    stats.lap(FrameStats::update, now_us());
    if (ps5_react_exit_requested()) {
      running = false;
      return true;
    }
    int sw = 0, sh = 0;
    SDL_GL_GetDrawableSize(window, &sw, &sh);
    if (sw > 0 && sh > 0) {
      if (!presenter.draw(er_software_framebuffer(), damage_tracker_rects(), sw, sh)) return false;
      damage_tracker_clear();
    }
    stats.lap(FrameStats::present, now_us());
    if (swap) SDL_GL_SwapWindow(window);
    stats.lap(FrameStats::swap, now_us());
    const Uint32 now = SDL_GetTicks();
    embedded_renderer_tick(std::min<Uint32>(now - previous_tick, 50));
    previous_tick = now;
    return !*er_runtime_last_error();
  }

  bool snapshot(const char* path) {
    if (!frame(false)) return false;
    int sw = 0, sh = 0;
    SDL_GL_GetDrawableSize(window, &sw, &sh);
    if (sw <= 0 || sh <= 0) return false;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(sw) * sh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    if (glGetError() != GL_NO_ERROR) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", sw, sh) > 0;
    for (int y = sh - 1; y >= 0 && ok; --y)
      ok = std::fwrite(rgb.data() + static_cast<std::size_t>(y) * sw * 3, 3, sw, file) == static_cast<std::size_t>(sw);
    if (std::fclose(file) != 0) ok = false;
    SDL_GL_SwapWindow(window);
    return ok;
  }
};

bool expect(int focus, int count, int detail) {
  JSContext* ctx = er_runtime_context();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue state = JS_GetPropertyStr(ctx, global, "__ps5ReactTestState");
  const char* fields[] = {"focus", "count", "detail"};
  const int expected[] = {focus, count, detail};
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    JSValue value = JS_GetPropertyStr(ctx, state, fields[i]);
    int32_t actual = -1;
    if (JS_ToInt32(ctx, &actual, value) || actual != expected[i]) {
      std::fprintf(stderr, "%s: expected %d, got %d\n", fields[i], expected[i], actual);
      ok = false;
    }
    JS_FreeValue(ctx, value);
  }
  JS_FreeValue(ctx, state); JS_FreeValue(ctx, global);
  return ok;
}

bool color_test(Host& host) {
  const std::uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  std::vector<std::uint32_t> pattern(width * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      pattern[y*width+x] = colors[(y >= height/2 ? 2 : 0) + (x >= width/2 ? 1 : 0)];
  int sw = 0, sh = 0;
  SDL_GL_GetDrawableSize(host.window, &sw, &sh);
  if (!host.presenter.draw(pattern.data(), sw, sh)) return false;
  // Samples in the centre of each quadrant test channel order and vertical orientation.
  for (int i = 0; i < 4; ++i) {
    unsigned char pixel[4] = {};
    glReadPixels((i%2 ? 3 : 1)*sw/4, (i/2 ? 1 : 3)*sh/4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const auto c = colors[i];
    if (pixel[0] != ((c>>16)&255) || pixel[1] != ((c>>8)&255) ||
        pixel[2] != (c&255) || pixel[3] != 255) return false;
  }
  return glGetError() == GL_NO_ERROR;
}

bool press(Host& host, SDL_Keycode key, int focus, int count, int detail) {
  SDL_Event event = {};
  event.type = SDL_KEYDOWN; event.key.keysym.sym = key;
  if (SDL_PushEvent(&event) != 1) return false;
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  return expect(focus, count, detail);
}

bool self_test(Host& host) {
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  if (!expect(0, 0, 0) || !host.snapshot("texture-initial.ppm")) return false;
  if (!press(host, SDLK_RETURN, 0, 1, 1) || !host.snapshot("texture-increment.ppm") ||
      !press(host, SDLK_BACKSPACE, 0, 1, 0) || !press(host, SDLK_RIGHT, 1, 1, 0) ||
      !press(host, SDLK_RETURN, 1, 11, 1) || !press(host, SDLK_BACKSPACE, 1, 11, 0) ||
      !press(host, SDLK_RIGHT, 2, 11, 0) || !press(host, SDLK_RETURN, 2, 0, 1) ||
      !press(host, SDLK_BACKSPACE, 2, 0, 0)) return false;
  SDL_Event button = {};
  button.type = SDL_CONTROLLERBUTTONDOWN;
  button.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  if (SDL_PushEvent(&button) != 1) return false;
  button.cbutton.button = SDL_CONTROLLER_BUTTON_A;
  if (SDL_PushEvent(&button) != 1 || !host.frame() || !expect(0, 1, 1)) return false;
  if (SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) return false;
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  if (!(SDL_GetWindowFlags(host.window) & SDL_WINDOW_FULLSCREEN) ||
      !expect(0, 1, 1) || !host.snapshot("texture-fullscreen.ppm")) return false;
  std::puts("PASS: software framebuffer → OpenGL texture, channel order, orientation, React state/focus, simulated controller and fullscreen.");
  return true;
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s bundle.js [--self-test|--fullscreen]\n", argv[0]);
    return 1;
  }
  const bool testing = argc > 2 && !std::strcmp(argv[2], "--self-test");
  Host host;
  host.log_frames = !testing;
  bool ok = host.start();
  if (ok && testing) ok = color_test(host);
  if (ok) ok = host.boot(argv[1]);
  if (ok && testing) ok = self_test(host);
  else if (ok) {
    if (argc > 2 && !std::strcmp(argv[2], "--fullscreen")) {
      ok = SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
      SDL_ShowCursor(SDL_DISABLE);
    }
    while (ok && host.running) ok = host.frame();
  }
  if (!ok) std::fprintf(stderr, "Texture proof failed: %s / %s\n", SDL_GetError(), er_runtime_last_error());
  return ok ? 0 : 1;
}
