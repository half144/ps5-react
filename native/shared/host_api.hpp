// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Native API contract (ABI v1) between the hosts and `@ps5-react/core`.
//
// host_api.cpp (shared) installs `globalThis.__ps5ReactNative` through
// ErRuntimeConfig.install_host_globals and implements the filesystem with POSIX
// (mount listing excepted).
// Each platform implements the `host::` functions below. Everything runs on the
// render thread; the functions are synchronous and must return quickly.
//
// JavaScript shape (runtime/js/native.js wraps it):
//   platform                       'ps5' | 'desktop'
//   fs.readDir(path)               [{name, isDirectory, isFile, size, modified}]
//   fs.stat(path)                  {name, isDirectory, isFile, size, modified} | null
//   fs.readFile(path)              string (UTF-8; at most kMaxReadBytes)
//   fs.writeFile(path, text, append)
//   fs.mkdir(path, recursive)
//   fs.remove(path)                file or empty directory
//   fs.rename(from, to)
//   fs.mounts()                    [{device, path, type}], visible to the process
//                                  console access is opt-in via app.json
//   fs.diskUsage(path)             {total, free} in bytes; free available to the app
//   device.info()                  {model, firmware, cpuTemperature, socTemperature,
//                                   cpuFrequency, freeMemory, processTime}; unknown = null
//   users.foreground()             {id, name} | null
//   users.loggedIn()               [{id, name}]
//   notify(message, subMessage)    boolean
//   openURL(url)                   boolean
//   pad.setLightBar(r, g, b)       0..255 each
//   pad.resetLightBar()
//   pad.vibrate(strength, seconds) strength 0..1
//   pad.state()                    {connected, leftX, leftY, rightX, rightY, l2, r2, buttons}
//   exit()                         asks the host to close after this frame
// Failures throw a JS Error whose message names the call, the path, and strerror.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "quickjs.h"
}

constexpr std::size_t kMaxReadBytes = 8 * 1024 * 1024;

// Shared (host_api.cpp).
void ps5_react_install_host_api(JSContext* ctx);
bool ps5_react_exit_requested();

// Latest controller state, set by the host once per frame before er_runtime_pump().
struct GamepadState {
  bool connected = false;
  float left_x = 0, left_y = 0, right_x = 0, right_y = 0; // -1..1, deadzone applied
  float l2 = 0, r2 = 0;                                   // 0..1
  std::uint32_t buttons = 0;                              // bit i = kButtonNames[i] held
};
inline constexpr const char* kButtonNames[] = {
  "up", "down", "left", "right", "cross", "circle", "triangle", "square",
  "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
};
void ps5_react_set_gamepad(const GamepadState& state);

// Per platform (native/ps5/host_platform.cpp, native/desktop/host_platform.cpp).
namespace host {
const char* platform_name();
// Maps an app path (/app0, /download0, /temp0, ...) to a real path. False rejects it.
bool resolve_path(const char* path, char* out, std::size_t size);

struct MountEntry {
  char device[128] = "";
  char path[128] = "";
  char type[16] = "";
};
// Fills at most max entries; returns the count, or -1 with errno set.
int list_mounts(MountEntry* mounts, int max);
// Calls visit for each entry name of a directory, skipping "." and "..";
// false with errno set when the directory cannot be read.
bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user);
// Total and free bytes of the filesystem holding real_path; false with errno set.
bool disk_usage(const char* real_path, double& total, double& free);
// Records the first use of each native call (PS5: kernel log), to locate native faults.
void trace(const char* call);

struct DeviceInfo {
  char model[64] = "";
  char firmware[32] = "";
  int cpu_temperature = -1;      // °C, -1 unknown
  int soc_temperature = -1;      // °C, -1 unknown
  std::int64_t cpu_frequency = -1; // Hz
  std::int64_t free_memory = -1;   // bytes available to the app
  std::int64_t process_time = -1;  // µs of CPU time used by this process
};
void device_info(DeviceInfo& info);

struct User {
  int id = -1;
  char name[32] = "";
};
bool foreground_user(User& user);
int logged_in_users(User* users, int max);

bool notify(const char* message, const char* sub_message);
bool open_url(const char* url);

void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
void reset_light_bar();
void vibrate(float strength, float seconds);
} // namespace host
