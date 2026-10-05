// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// PS5 side of the native API (host_api.hpp) through user-level libkernel and
// libSce* calls. None of these calls has been verified on hardware yet.

#include "host_api.hpp"
#include "host_platform.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
// The firmware writes at most size bytes, so size must be set before the call.
struct SystemSwVersion {
  std::uint64_t size;
  char version_string[0x1c];
  std::uint32_t version;
};

struct LoginUserIdList {
  int user_id[4];
};

constexpr int kInvalidUserId = -1;
constexpr int kNotificationSystemUser = 0xFE;
}

extern "C" {
int sceKernelGetSystemSwVersion(SystemSwVersion* version);
int sceKernelGetCpuTemperature(int* temperature);
int sceKernelGetSocSensorTemperature(int sensor, int* temperature);
long sceKernelGetCpuFrequency(void);
int sceKernelAvailableFlexibleMemorySize(std::size_t* size);
std::uint64_t sceKernelGetProcessTime(void);
int sceUserServiceGetForegroundUser(int* user);
int sceUserServiceGetUserName(int user, char* name, std::size_t size);
int sceUserServiceGetLoginUserIdList(LoginUserIdList* list);
int sceSystemServiceLaunchWebBrowser(const char* uri, void* parameters);
int sceKernelOpen(const char* path, int flags, int mode);
int sceKernelGetdents(int fd, char* buffer, int size);
int sceKernelClose(int fd);
}

namespace {
hui::ps5::Pad* active_pad = nullptr;

// Resolves a function from a module the title does not link, on first use. Only
// the modules the platform already needs are linked: a NEEDED module the title
// cannot load would stop it from starting, while a failed lookup here disables
// one call. The handle stays open for the title's lifetime.
template <typename Function>
class OptionalSymbol {
public:
  constexpr OptionalSymbol(const char* module, const char* name) : module_(module), name_(name) {}
  Function get() {
    if (!resolved_) {
      resolved_ = true;
      void* handle = dlopen(module_, RTLD_LAZY);
      void* symbol = handle ? dlsym(handle, name_) : nullptr;
      if (!symbol) hui::sys::log("[PS5-REACT] %s unavailable from %s", name_, module_);
      function_ = reinterpret_cast<Function>(symbol);
    }
    return function_;
  }

private:
  const char* module_;
  const char* name_;
  bool resolved_ = false;
  Function function_ = nullptr;
};

OptionalSymbol<int (*)(int, bool, const char*)> notification_send{"libSceNotification.sprx", "sceNotificationSend"};
OptionalSymbol<int (*)(char*)> hw_model_name{"libkernel_sys.sprx", "sceKernelGetHwModelName"};
OptionalSymbol<int (*)(struct statfs*, long, int)> get_fs_stat{"libkernel_sys.sprx", "getfsstat"};
// Not statvfs: a title crashed when System Explorer's Files tab first ran it (suspected cause).
OptionalSymbol<int (*)(const char*, struct statfs*)> stat_fs{"libkernel_sys.sprx", "statfs"};

// Writes text as the body of a JSON string. False if it does not fit.
bool json_escape(const char* text, char* out, std::size_t size) {
  std::size_t used = 0;
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    char escaped[8];
    int length;
    if (*c == '"' || *c == '\\') length = std::snprintf(escaped, sizeof escaped, "\\%c", *c);
    else if (*c < 0x20) length = std::snprintf(escaped, sizeof escaped, "\\u%04x", *c);
    else { escaped[0] = static_cast<char>(*c); length = 1; }
    if (used + static_cast<std::size_t>(length) >= size) return false;
    std::memcpy(out + used, escaped, static_cast<std::size_t>(length));
    used += static_cast<std::size_t>(length);
  }
  out[used] = '\0';
  return true;
}

// ISO 8601 UTC from CLOCK_REALTIME without gmtime, which the app libc may lack
// (days-to-civil conversion from Howard Hinnant's date algorithms).
void utc_timestamp(char* out, std::size_t size) {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  const std::int64_t seconds = now.tv_sec;
  const std::int64_t days = seconds / 86400, rest = seconds % 86400;
  const std::int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  const std::int64_t day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = yoe + era * 400 + (month <= 2);
  std::snprintf(out, size, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03ldZ",
                static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
                static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60),
                static_cast<long long>(rest % 60), now.tv_nsec / 1000000);
}

bool user_name(int id, char* name, std::size_t size) {
  const int result = sceUserServiceGetUserName(id, name, size);
  if (result != 0) {
    hui::sys::log("[PS5-REACT] sceUserServiceGetUserName=0x%08x", static_cast<unsigned>(result));
    name[0] = '\0';
  }
  return result == 0;
}
}

void host_platform_set_pad(hui::ps5::Pad* pad) { active_pad = pad; }

namespace host {
const char* platform_name() { return "ps5"; }

// The title is sandboxed (/app0 read-only, /download0 writable, /temp0), so app
// paths are already real paths; only parent traversal is refused.
bool resolve_path(const char* path, char* out, std::size_t size) {
  if (path[0] != '/') return false;
  for (const char* part = path; part; part = std::strchr(part + 1, '/'))
    if (part[1] == '.' && part[2] == '.' && (part[3] == '/' || part[3] == '\0')) return false;
  const std::size_t length = std::strlen(path);
  if (length >= size) return false;
  std::memcpy(out, path, length + 1);
  return true;
}

int list_mounts(MountEntry* mounts, int max) {
  const auto getfsstat = get_fs_stat.get();
  if (!getfsstat) {
    errno = ENOSYS;
    return -1;
  }
  static struct statfs found[64];
  const int count = getfsstat(found, sizeof found, MNT_NOWAIT);
  if (count < 0) return -1;
  const int used = count < max ? count : max;
  for (int i = 0; i < used; ++i) {
    std::snprintf(mounts[i].device, sizeof mounts[i].device, "%s", found[i].f_mntfromname);
    std::snprintf(mounts[i].path, sizeof mounts[i].path, "%s", found[i].f_mntonname);
    std::snprintf(mounts[i].type, sizeof mounts[i].type, "%s", found[i].f_fstypename);
  }
  return used;
}

bool disk_usage(const char* real_path, double& total, double& free) {
  const auto statfs_fn = stat_fs.get();
  if (!statfs_fn) {
    errno = ENOSYS;
    return false;
  }
  struct statfs fs;
  if (statfs_fn(real_path, &fs) != 0) return false;
  total = static_cast<double>(fs.f_bsize) * static_cast<double>(fs.f_blocks);
  free = static_cast<double>(fs.f_bsize) * static_cast<double>(fs.f_bavail);
  return true;
}

void trace(const char* call) { hui::sys::log("[PS5-REACT] native %s", call); }

// libkernel's sce* file calls return SCE_KERNEL_ERROR_* (0x8002xxxx, errno in the low bits).
bool sce_failed(int result) {
  if (result >= 0) return false;
  errno = static_cast<int>(static_cast<unsigned>(result) & 0xffffu);
  return true;
}

// sceKernelGetdents, as titles list directories; opendir from libSceLibcInternal
// fails with EPERM inside a title sandbox.
bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user) {
  const int fd = sceKernelOpen(real_path, O_RDONLY | O_DIRECTORY, 0);
  if (sce_failed(fd)) return false;
  alignas(8) static char buffer[8192];
  int read = 0;
  while (!sce_failed(read = sceKernelGetdents(fd, buffer, sizeof buffer)) && read > 0) {
    for (int offset = 0; offset < read;) {
      const auto* item = reinterpret_cast<const dirent*>(buffer + offset);
      if (item->d_reclen == 0) break;
      if (item->d_fileno != 0 && std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, ".."))
        visit(item->d_name, user);
      offset += item->d_reclen;
    }
  }
  const int saved = errno;
  sceKernelClose(fd);
  errno = saved;
  return read == 0;
}

void device_info(DeviceInfo& info) {
  if (const auto model_name = hw_model_name.get()) {
    char model[1024] = "";
    if (model_name(model) == 0) std::snprintf(info.model, sizeof info.model, "%s", model);
  }
  SystemSwVersion version{};
  version.size = sizeof version;
  if (sceKernelGetSystemSwVersion(&version) == 0)
    std::snprintf(info.firmware, sizeof info.firmware, "%.*s",
                  static_cast<int>(sizeof version.version_string), version.version_string);
  int temperature = 0;
  if (sceKernelGetCpuTemperature(&temperature) == 0) info.cpu_temperature = temperature;
  if (sceKernelGetSocSensorTemperature(0, &temperature) == 0) info.soc_temperature = temperature;
  const long frequency = sceKernelGetCpuFrequency();
  if (frequency > 0) info.cpu_frequency = frequency;
  std::size_t free_memory = 0;
  if (sceKernelAvailableFlexibleMemorySize(&free_memory) == 0)
    info.free_memory = static_cast<std::int64_t>(free_memory);
  info.process_time = static_cast<std::int64_t>(sceKernelGetProcessTime());
}

// Pad::open() initializes the user service before the bundle runs.
bool foreground_user(User& user) {
  int id = kInvalidUserId;
  if (sceUserServiceGetForegroundUser(&id) != 0 || id == kInvalidUserId) return false;
  user.id = id;
  user_name(id, user.name, sizeof user.name);
  return true;
}

int logged_in_users(User* users, int max) {
  LoginUserIdList list{};
  if (sceUserServiceGetLoginUserIdList(&list) != 0) return 0;
  int count = 0;
  for (int id : list.user_id) {
    if (id == kInvalidUserId || count == max) continue;
    users[count].id = id;
    user_name(id, users[count].name, sizeof users[count].name);
    ++count;
  }
  return count;
}

bool notify(const char* message, const char* sub_message) {
  char body[512], sub_body[512], created[32], payload[2048];
  if (!json_escape(message, body, sizeof body) || !json_escape(sub_message, sub_body, sizeof sub_body))
    return false;
  utc_timestamp(created, sizeof created);
  const int length = std::snprintf(payload, sizeof payload,
    "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
    "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
    "\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
    "\"message\":{\"body\":\"%s\"},\"subMessage\":{\"body\":\"%s\"}}},"
    "\"createdDateTime\":\"%s\",\"localNotificationId\":\"%lld\"}",
    body, sub_body, created, static_cast<long long>(hui::sys::monotonic_us() % 1000000000));
  const auto send = notification_send.get();
  if (!send || length < 0 || static_cast<std::size_t>(length) >= sizeof payload) return false;
  const int result = send(kNotificationSystemUser, true, payload);
  if (result != 0) hui::sys::log("[PS5-REACT] sceNotificationSend=0x%08x", static_cast<unsigned>(result));
  return result == 0;
}

bool open_url(const char* url) {
  if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) return false;
  const int result = sceSystemServiceLaunchWebBrowser(url, nullptr);
  if (result != 0)
    hui::sys::log("[PS5-REACT] sceSystemServiceLaunchWebBrowser=0x%08x", static_cast<unsigned>(result));
  return result == 0;
}

void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  if (active_pad) active_pad->set_light_bar(r, g, b);
}

// Pad caches the last colour, so the reset goes through it rather than
// scePadResetLightBar; the default is the first player's blue.
void reset_light_bar() { set_light_bar(0, 0, 255); }

void vibrate(float strength, float seconds) {
  if (!active_pad) return;
  // Pad::rumble ignores zero; a strength below 1/255 writes stopped motors and
  // still arms tick(), which clears it.
  if (strength <= 0.0f || seconds <= 0.0f) active_pad->rumble(0.001f, 0.001f);
  else active_pad->rumble(strength, seconds);
}
} // namespace host
