// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "filesystem_access.hpp"
#include "app_config.hpp"
#include "host_platform.hpp"
#include "platform/ps5/system.hpp"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if PS5_REACT_CONSOLE_FILESYSTEM
#include "elevation.hpp"
extern "C" {
int sceNetInit(void);
int sceNetTerm(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
}
#endif

namespace {
bool console_access = false;
char app_root[256] = "/app0";
char data_root[256] = "/download0";
char temp_root[256] = "/temp0";

#if PS5_REACT_CONSOLE_FILESYSTEM
// Native titles must initialize libSceNet; linking its stub is not enough.
// The resident/helper client owns its sockets; this scope owns only our pool
// and library initialization, never a pool or initialization owned elsewhere.
struct Network {
  int init = sceNetInit();
  int pool = sceNetPoolCreate("ps5-react-filesystem", 0x10000, 0);
  Network() {
    hui::sys::log("[PS5-REACT] filesystem network init=0x%08x pool=%d",
                  static_cast<unsigned>(init), pool);
  }
  ~Network() {
    if (pool >= 0) sceNetPoolDestroy(pool);
    if (init == 0) sceNetTerm();
  }
};

bool accessible(const char* path) {
  const int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
  if (fd < 0) return false;
  close(fd);
  return true;
}

// Preserve logical app roots after the helper changes the process's root.
// Prefer the original sandbox mounts. /data is used only after its write/read
// proof succeeded, and is namespaced by title rather than replacing app files.
void sandbox_root(const char* name, char* out, std::size_t size) {
  char path[256];
  std::snprintf(path, sizeof path, "/%s", name);
  if (!accessible(path))
    std::snprintf(path, sizeof path, "/mnt/sandbox/%s_000/%s", PS5_REACT_TITLE, name);
  std::snprintf(out, size, "%s", path);
}
#endif
}

void initialize_filesystem_access() {
#if PS5_REACT_CONSOLE_FILESYSTEM
  host_platform_prepare_filesystem_access();
  const auto status = [] {
    Network network;
    // An already-initialized library can return an error from sceNetInit;
    // successful pool creation establishes that sockets can be attempted.
    if (network.pool < 0) return elevation::Status::transport_error;
    return elevation::request(elevation::Capability::filesystem);
  }();
  hui::sys::log("[PS5-REACT] filesystem access status=%u path=%s",
                static_cast<unsigned>(status), elevation::path());
  if (status != elevation::Status::ok) return;
  console_access = true;
  sandbox_root("app0", app_root, sizeof app_root);
  sandbox_root("download0", data_root, sizeof data_root);
  sandbox_root("temp0", temp_root, sizeof temp_root);
  if (!accessible(data_root)) {
    mkdir("/data/ps5-react", 0700);
    std::snprintf(data_root, sizeof data_root, "/data/ps5-react/%s", PS5_REACT_TITLE);
    mkdir(data_root, 0700);
  }
  if (!accessible(temp_root)) {
    std::snprintf(temp_root, sizeof temp_root, "%s/tmp", data_root);
    mkdir(temp_root, 0700);
  }
  hui::sys::log("[PS5-REACT] filesystem roots app=%s data=%s temp=%s", app_root, data_root, temp_root);
#endif
}

bool resolve_filesystem_path(const char* path, char* out, std::size_t size) {
  const char* suffix = "";
  const char* base = path;
  if (console_access) {
    const char* logical[] = {"/app0", "/download0", "/temp0"};
    const char* physical[] = {app_root, data_root, temp_root};
    for (int i = 0; i < 3; ++i) {
      const auto length = std::strlen(logical[i]);
      if (!std::strncmp(path, logical[i], length) && (!path[length] || path[length] == '/')) {
        base = physical[i];
        suffix = path + length;
        break;
      }
    }
  }
  const int length = std::snprintf(out, size, "%s%s", base, suffix);
  return length >= 0 && static_cast<std::size_t>(length) < size;
}

bool console_filesystem_accessible() { return console_access; }
