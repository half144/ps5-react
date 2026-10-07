// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "filesystem_access.hpp"
#include "app_config.hpp"
#include "host_platform.hpp"
#include "platform/ps5/system.hpp"
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
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
// Caches that may grow large: the title's download0 is a fixed image of a few hundred MiB that also holds
// its state, so a full cache there left the app nowhere to write.
char cache_root[256] = "/download0/.cache";

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

// A sandbox mount can remain readable after elevation while denying writes.
// Exercise the same file operations as the app, without replacing any file.
bool writable(const char* root) {
  char path[512];
  int fd = -1;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (std::snprintf(path, sizeof path, "%s/.ps5-react-write-%ld-%d", root,
                      static_cast<long>(getpid()), attempt) >= static_cast<int>(sizeof path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) return false;
  const char token = 'R';
  ssize_t count;
  do { count = write(fd, &token, 1); } while (count < 0 && errno == EINTR);
  bool ok = count == 1;
  int error = ok ? 0 : count < 0 ? errno : EIO;
  if (close(fd) != 0 && ok) { ok = false; error = errno; }
  if (ok) {
    fd = open(path, O_RDONLY, 0);
    char received = 0;
    if (fd < 0) { ok = false; error = errno; }
    else {
      do { count = read(fd, &received, 1); } while (count < 0 && errno == EINTR);
      ok = count == 1 && received == token;
      if (!ok) error = count < 0 ? errno : EIO;
    }
    if (fd >= 0 && close(fd) != 0 && ok) { ok = false; error = errno; }
  }
  if (unlink(path) != 0 && ok) { ok = false; error = errno; }
  if (!ok) errno = error ? error : EIO;
  return ok;
}

// Images were cached in download0 until it filled the title's fixed-size image and left no room for
// its state; they now live under /data, and the old files go before download0 is checked.
void remove_legacy_image_cache() {
  char directory[320];
  std::snprintf(directory, sizeof directory, "%s/.cache/images", data_root);
  DIR* dir = opendir(directory);
  if (!dir) return;
  int removed = 0;
  while (dirent* item = readdir(dir)) {
    if (item->d_name[0] == '.') continue;
    char path[600];
    std::snprintf(path, sizeof path, "%s/%s", directory, item->d_name);
    removed += unlink(path) == 0;
  }
  closedir(dir);
  rmdir(directory);
  hui::sys::log("[PS5-REACT] removed %d cached images from %s", removed, directory);
}

bool ensure_directory(const char* path) {
  if (mkdir(path, 0700) == 0) return true;
  return errno == EEXIST && accessible(path);
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
  remove_legacy_image_cache();
  if (!writable(data_root)) {
    hui::sys::log("[PS5-REACT] data root not writable path=%s errno=%d; trying title data", data_root, errno);
    std::snprintf(data_root, sizeof data_root, "/data/ps5-react/%s", PS5_REACT_TITLE);
    if (!ensure_directory("/data/ps5-react") || !ensure_directory(data_root) || !writable(data_root))
      hui::sys::log("[PS5-REACT] data root write proof failed path=%s errno=%d", data_root, errno);
  }
  if (!writable(temp_root)) {
    std::snprintf(temp_root, sizeof temp_root, "%s/tmp", data_root);
    if (!ensure_directory(temp_root) || !writable(temp_root))
      hui::sys::log("[PS5-REACT] temp root write proof failed path=%s errno=%d", temp_root, errno);
  }
  char title_data[256];
  std::snprintf(title_data, sizeof title_data, "/data/ps5-react/%s", PS5_REACT_TITLE);
  std::snprintf(cache_root, sizeof cache_root, "%s/cache", title_data);
  if (!ensure_directory("/data/ps5-react") || !ensure_directory(title_data) || !ensure_directory(cache_root) || !writable(cache_root))
    std::snprintf(cache_root, sizeof cache_root, "%s/.cache", data_root);
  hui::sys::log("[PS5-REACT] filesystem roots app=%s data=%s temp=%s cache=%s", app_root, data_root, temp_root, cache_root);
#endif
}

bool resolve_filesystem_path(const char* path, char* out, std::size_t size) {
  const char* suffix = "";
  const char* base = path;
  if (!std::strncmp(path, "/cache0", 7) && (!path[7] || path[7] == '/')) {
    // Without console access the sandbox paths are used as they are.
    base = console_access ? cache_root : "/download0/.cache";
    suffix = path + 7;
  } else if (console_access) {
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
