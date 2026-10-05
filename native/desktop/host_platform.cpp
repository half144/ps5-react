// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"
#include "host_platform.hpp"

#include <dirent.h>
#include <mach/mach.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace {
SDL_GameController* controller = nullptr;

void sysctl_string(const char* name, char* out, std::size_t size) {
  if (sysctlbyname(name, out, &size, nullptr, 0) != 0) *out = '\0';
}

std::int64_t free_memory() {
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page = 0;
  if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
      host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS)
    return -1;
  return static_cast<std::int64_t>(stats.free_count + stats.inactive_count) * static_cast<std::int64_t>(page);
}
} // namespace

void desktop_set_controller(SDL_GameController* value) {
  controller = value;
}

namespace host {
const char* platform_name() {
  return "desktop";
}

int list_mounts(MountEntry* mounts, int max) {
  struct statfs* found = nullptr;
  const int count = getmntinfo(&found, MNT_NOWAIT);
  if (count <= 0) return -1;
  const int used = count < max ? count : max;
  for (int i = 0; i < used; ++i) {
    std::snprintf(mounts[i].device, sizeof mounts[i].device, "%s", found[i].f_mntfromname);
    std::snprintf(mounts[i].path, sizeof mounts[i].path, "%s", found[i].f_mntonname);
    std::snprintf(mounts[i].type, sizeof mounts[i].type, "%s", found[i].f_fstypename);
  }
  return used;
}

bool disk_usage(const char* real_path, double& total, double& free) {
  struct statvfs fs;
  if (statvfs(real_path, &fs) != 0) return false;
  total = static_cast<double>(fs.f_frsize) * static_cast<double>(fs.f_blocks);
  free = static_cast<double>(fs.f_frsize) * static_cast<double>(fs.f_bavail);
  return true;
}

void trace(const char*) {}

bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user) {
  DIR* dir = opendir(real_path);
  if (!dir) return false;
  while (const dirent* item = readdir(dir))
    if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) visit(item->d_name, user);
  closedir(dir);
  return true;
}

bool resolve_path(const char* path, char* out, std::size_t size) {
  if (path[0] != '/' || std::strstr(path, "/../")) return false;
  const std::size_t length = std::strlen(path);
  if (length >= 3 && !std::strcmp(path + length - 3, "/..")) return false;
  const char* root = std::getenv("PS5_REACT_SANDBOX");
  if (!root || !*root) root = "./sandbox";
  const int written = std::snprintf(out, size, "%s%s", root, length == 1 ? "" : path);
  return written >= 0 && static_cast<std::size_t>(written) < size;
}

void device_info(DeviceInfo& info) {
  char model[48];
  sysctl_string("hw.model", model, sizeof model);
  std::snprintf(info.model, sizeof info.model, "Desktop preview%s%s%s", *model ? " (" : "", model, *model ? ")" : "");
  char version[24];
  sysctl_string("kern.osproductversion", version, sizeof version);
  if (*version) std::snprintf(info.firmware, sizeof info.firmware, "macOS %s", version);
  std::uint64_t frequency = 0;
  std::size_t size = sizeof frequency;
  // Apple silicon does not publish a CPU frequency; it stays unknown there.
  if (sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0)
    info.cpu_frequency = static_cast<std::int64_t>(frequency);
  info.free_memory = free_memory();
  rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    info.process_time = (static_cast<std::int64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000 +
                        usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

bool foreground_user(User& user) {
  const char* name = std::getenv("USER");
  user.id = 1;
  std::snprintf(user.name, sizeof user.name, "%s", name && *name ? name : "player");
  return true;
}

int logged_in_users(User* users, int max) {
  return max > 0 && foreground_user(users[0]) ? 1 : 0;
}

bool notify(const char* message, const char* sub_message) {
  std::printf("[notify] %s%s%s\n", message, *sub_message ? ": " : "", sub_message);
  std::fflush(stdout);
  return true;
}

bool open_url(const char* url) {
  if (std::strncmp(url, "http://", 7) && std::strncmp(url, "https://", 8)) return false;
  char open[] = "open";
  char* argv[] = {open, const_cast<char*>(url), nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, "open", nullptr, nullptr, argv, environ) != 0) return false;
  int status = 0;
  // `open` hands the URL to Launch Services and exits at once.
  return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  if (!controller || SDL_GameControllerSetLED(controller, r, g, b) != 0)
    std::printf("[pad] light bar %u,%u,%u (no controller LED)\n", r, g, b);
}

void reset_light_bar() {
  // SDL has no reset; restore the DualSense player-one blue.
  if (!controller || SDL_GameControllerSetLED(controller, 0, 0, 64) != 0)
    std::puts("[pad] light bar reset (no controller LED)");
}

void vibrate(float strength, float seconds) {
  const auto level = static_cast<Uint16>(strength * 0xffff);
  if (!controller || SDL_GameControllerRumble(controller, level, level, static_cast<Uint32>(seconds * 1000)) != 0)
    std::printf("[pad] vibrate %.2f for %.2fs (no controller rumble)\n", strength, seconds);
}
} // namespace host
