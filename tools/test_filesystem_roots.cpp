// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fixture {
std::map<std::string, bool> directories;
std::map<std::string, char> files;
std::map<int, std::string> handles;
int next_fd = 10;
bool corrupt_read = false;

int open(const char* path, int flags, int) {
  const std::string name = path;
  if (flags & O_DIRECTORY) {
    if (!directories.contains(name)) { errno = ENOENT; return -1; }
  } else if (flags & O_CREAT) {
    const auto parent = name.substr(0, name.rfind('/'));
    if (!directories.contains(parent)) { errno = ENOENT; return -1; }
    if (!directories.at(parent)) { errno = EACCES; return -1; }
    if (files.contains(name)) { errno = EEXIST; return -1; }
    files[name] = 0;
  } else if (!files.contains(name)) { errno = ENOENT; return -1; }
  handles[next_fd] = name;
  return next_fd++;
}
int close(int fd) { handles.erase(fd); return 0; }
ssize_t write(int fd, const void* data, std::size_t) {
  files.at(handles.at(fd)) = *static_cast<const char*>(data);
  return 1;
}
ssize_t read(int fd, void* data, std::size_t) {
  *static_cast<char*>(data) = corrupt_read ? '?' : files.at(handles.at(fd));
  return 1;
}
int unlink(const char* path) { return files.erase(path) ? 0 : -1; }
int mkdir(const char* path, mode_t) {
  const std::string name = path;
  if (directories.contains(name)) { errno = EEXIST; return -1; }
  const auto parent = name.substr(0, name.rfind('/'));
  if (!directories.contains(parent)) { errno = ENOENT; return -1; }
  if (!directories.at(parent)) { errno = EACCES; return -1; }
  directories[name] = true;
  return 0;
}
pid_t getpid() { return 123; }
}

#define open fixture::open
#define close fixture::close
#define write fixture::write
#define read fixture::read
#define unlink fixture::unlink
#define mkdir fixture::mkdir
#define getpid fixture::getpid
#include "../native/ps5/filesystem_access.cpp"
#undef open
#undef close
#undef write
#undef read
#undef unlink
#undef mkdir
#undef getpid

elevation::Status status = elevation::Status::ok;
elevation::Status elevation::request(Capability, const char*) noexcept { return status; }
void crash_log::start(const char*) {}
const char* elevation::path() noexcept { return "fixture"; }
extern "C" int sceNetInit() { return 0; }
extern "C" int sceNetTerm() { return 0; }
extern "C" int sceNetPoolCreate(const char*, int, int) { return 1; }
extern "C" int sceNetPoolDestroy(int) { return 0; }
void host_platform_prepare_filesystem_access() {}

void reset(bool writable_sandbox) {
  fixture::directories = {{"/app0", true}, {"/download0", writable_sandbox},
                          {"/temp0", writable_sandbox}, {"/data", true}};
  fixture::files.clear();
  fixture::handles.clear();
  fixture::corrupt_read = false;
  console_access = false;
  std::strcpy(data_root, "/download0");
  std::strcpy(temp_root, "/temp0");
  status = elevation::Status::ok;
}

void expect_path(const char* logical, const char* expected) {
  char result[512];
  assert(resolve_filesystem_path(logical, result, sizeof result));
  assert(std::strcmp(result, expected) == 0);
}

int main() {
  reset(true);
  initialize_filesystem_access();
  expect_path("/download0/overdrive/downloads.json.tmp", "/download0/overdrive/downloads.json.tmp");
  assert(fixture::files.empty() && fixture::handles.empty());

  reset(false);
  initialize_filesystem_access();
  expect_path("/download0/overdrive/downloads.json.tmp", "/data/ps5-react/PPSA99058/overdrive/downloads.json.tmp");
  expect_path("/temp0/test", "/data/ps5-react/PPSA99058/tmp/test");
  assert(console_filesystem_accessible());
  assert(fixture::files.empty() && fixture::handles.empty());

  reset(true);
  fixture::files["/download0/.ps5-react-write-123-0"] = 'X';
  assert(writable("/download0"));
  assert(fixture::files.size() == 1 && fixture::files.begin()->second == 'X');

  reset(true);
  fixture::corrupt_read = true;
  assert(!writable("/download0"));
  assert(fixture::files.empty() && fixture::handles.empty());

  reset(false);
  status = elevation::Status::transport_error;
  initialize_filesystem_access();
  assert(!console_filesystem_accessible());
  assert(!fixture::directories.contains("/data/ps5-react"));
  expect_path("/download0/file", "/download0/file");
  std::puts("PASS: writable roots, read-only fallback, exclusive probes, failed proof and failed elevation");
}
