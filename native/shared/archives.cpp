// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "archives.hpp"
#include "archive_preflight.hpp"
#include "digest.hpp"
#include "thread_name.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace archives {
namespace {
constexpr std::size_t max_artifacts = 64, max_path_length = 4096, read_block_size = 128 * 1024;
constexpr unsigned max_depth = 128;
// ShadowMount mounts these images; packages are installed by the console.
constexpr std::array<std::string_view, 4> image_extensions{".ffpfsc", ".ffpfs", ".ffpkg", ".exfat"};
constexpr std::array<std::string_view, 2> package_extensions{".pkg", ".fpkg"};
const char* receipt_name = ".ps5-react-extraction";
const char* stage_name = ".ps5-react-stage";

struct Job {
  Request request;
  Snapshot snapshot;
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::thread worker;
};
std::mutex guard;
std::shared_ptr<Job> active;
std::uint32_t next_id = 1;

bool terminal(const std::string& state) { return state == "completed" || state == "failed" || state == "cancelled"; }
void fail(Job& job, const std::string& error) {
  std::lock_guard lock(job.mutex);
  job.snapshot.error = error;
  job.snapshot.state = job.cancelled ? "cancelled" : "failed";
}
template <std::size_t count>
bool has_extension(const std::string& path, const std::array<std::string_view, count>& extensions) {
  return std::any_of(extensions.begin(), extensions.end(), [&](std::string_view extension) { return path.ends_with(extension); });
}
bool image(const std::string& path) { return has_extension(path, image_extensions); }
bool installable(const std::string& path) { return image(path) || has_extension(path, package_extensions); }
const char* archive_error(archive* archive, const char* fallback) {
  const char* error = archive_error_string(archive);
  return error ? error : fallback;
}

bool safe(const char* name) {
  if (!name || !*name || *name == '/' || std::strlen(name) > max_path_length) return false;
  if (std::strchr(name, '\\') || std::strchr(name, ':')) return false;
  const std::string path(name);
  for (unsigned char c : path) {
    if (c < 32 || c == 127 || c == '|') return false;
  }
  std::size_t start = 0;
  unsigned depth = 0;
  while (start < path.size()) {
    auto end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == ".." || ++depth > max_depth) return false;
    start = end + 1;
  }
  return true;
}
// Every directory is opened relative to an owned root, never following archive-created links.
int parent_fd(int root, const std::string& name, std::string& leaf) {
  int fd = dup(root);
  std::size_t start = 0;
  while (true) {
    const auto end = name.find('/', start);
    if (end == std::string::npos) {
      leaf = name.substr(start);
      return fd;
    }
    const auto part = name.substr(start, end - start);
    start = end + 1;
    if (part == ".") continue;
    if (mkdirat(fd, part.c_str(), 0700) && errno != EEXIST) {
      close(fd);
      return -1;
    }
    const int child = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close(fd);
    if (child < 0) return -1;
    fd = child;
  }
}
bool file_hash(const std::string& path, Job& job, std::string& digest) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return false;
  integrity::Hash hash;
  std::array<char, read_block_size> buffer{};
  bool ok = true;
  while (!job.cancelled) {
    const auto bytes = read(fd, buffer.data(), buffer.size());
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes < 0) {
      ok = false;
      break;
    }
    if (!bytes) break;
    if (!hash.update(buffer.data(), bytes)) {
      ok = false;
      break;
    }
  }
  close(fd);
  digest = hash.finish();
  return ok && !job.cancelled && digest.size() == 64;
}
std::string input_identity(const Request& request) {
  std::string identity;
  for (const auto& path : request.sources) {
    struct stat st;
    if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return {};
#ifdef __APPLE__
    const auto time = st.st_mtimespec;
#else
    const auto time = st.st_mtim;
#endif
    identity += path + "|" + std::to_string(st.st_dev) + "|" + std::to_string(st.st_ino) + "|" + std::to_string(st.st_size);
    identity += "|" + std::to_string(time.tv_sec) + "|" + std::to_string(time.tv_nsec) + "\n";
  }
  integrity::Hash hash;
  hash.update(identity.data(), identity.size());
  return hash.finish();
}
// Bounded metadata I/O avoids libc++ locale machinery unavailable in native titles.
bool read_metadata(const std::string& path, std::string& text) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_metadata_bytes) {
    close(fd);
    return false;
  }
  text.resize(st.st_size);
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto count = read(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      close(fd);
      return false;
    }
    offset += count;
  }
  close(fd);
  return true;
}
bool write_metadata(int root, const char* name, const std::string& text) {
  const int fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  bool ok = true;
  while (offset < text.size()) {
    const auto count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += count;
  }
  if (ok && fsync(fd)) ok = false;
  close(fd);
  return ok;
}
// Remove only entries reached through the owned directory descriptor, without following links.
bool clear_directory(int root, unsigned depth = 0) {
  if (depth > max_depth) return false;
  DIR* directory = fdopendir(dup(root));
  if (!directory) return false;
  bool ok = true;
  while (auto* entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat st;
    if (fstatat(root, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
      ok = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      const int child = openat(root, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (child < 0) {
        ok = false;
        break;
      }
      ok = clear_directory(child, depth + 1);
      close(child);
      if (!ok || unlinkat(root, entry->d_name, AT_REMOVEDIR)) {
        ok = false;
        break;
      }
    } else if (unlinkat(root, entry->d_name, 0)) {
      ok = false;
      break;
    }
  }
  closedir(directory);
  return ok;
}
// True when every component below root is a real directory or, last, the entry itself.
bool plain_path(const std::string& root, const std::string& path) {
  std::string component = root;
  std::size_t begin = 0;
  while (begin < path.size()) {
    auto end = path.find('/', begin);
    if (end == std::string::npos) end = path.size();
    component += "/" + path.substr(begin, end - begin);
    struct stat node;
    if (lstat(component.c_str(), &node) || S_ISLNK(node.st_mode)) return false;
    if (end < path.size() && !S_ISDIR(node.st_mode)) return false;
    begin = end + 1;
  }
  return true;
}
bool recover(Job& job, const std::string& identity) {
  const auto root = job.request.destination;
  struct stat st;
  const auto receipt = root + "/" + receipt_name;
  if (lstat(receipt.c_str(), &st) || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > max_metadata_bytes) return false;
  std::string metadata;
  if (!read_metadata(receipt, metadata)) return false;
  std::size_t cursor = 0;
  auto next_line = [&](std::string& line) {
    const auto end = metadata.find('\n', cursor);
    if (end == std::string::npos) return false;
    line = metadata.substr(cursor, end - cursor);
    cursor = end + 1;
    return true;
  };
  std::string line;
  if (!next_line(line) || line != "P5AR001:" + identity) return false;
  Snapshot verified;
  while (next_line(line)) {
    const auto split = line.find('|');
    if (split != 64) return false;
    const auto expected = line.substr(0, split), path = line.substr(split + 1);
    if (!safe(path.c_str()) || !plain_path(root, path)) return false;
    std::string actual;
    if (!file_hash(root + "/" + path, job, actual) || actual != expected) return false;
    if (lstat((root + "/" + path).c_str(), &st) || !S_ISREG(st.st_mode)) return false;
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > job.request.max_bytes - verified.written) return false;
    if (++verified.entries > max_entries) return false;
    verified.written += st.st_size;
    if (installable(path)) verified.artifacts.push_back(path);
    if (verified.artifacts.size() > max_artifacts) return false;
  }
  if (cursor != metadata.size()) return false;
  std::lock_guard lock(job.mutex);
  job.snapshot.written = verified.written;
  job.snapshot.entries = verified.entries;
  job.snapshot.artifacts = std::move(verified.artifacts);
  return true;
}
// Removes staging left by an interrupted run of this same extraction.
bool restart_staging(const std::string& staging, const struct stat& expected) {
  const int previous = open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  struct stat opened;
  const bool owned = previous >= 0 && !fstat(previous, &opened) && opened.st_dev == expected.st_dev && opened.st_ino == expected.st_ino;
  const bool cleared = owned && clear_directory(previous);
  if (previous >= 0) close(previous);
  return cleared && !rmdir(staging.c_str());
}
// The SHA-256 is taken from the blocks as they are written. A file that arrives out of order or with
// holes is hashed by reading it back.
std::string write_entry(Job& job, archive* archive, int fd, std::int64_t size, const std::string& path, std::string& digest) {
  const auto max_bytes = job.request.max_bytes;
  const void* buffer = nullptr;
  std::size_t bytes = 0;
  la_int64_t offset = 0;
  std::uint64_t extent = 0, hashed = 0;
  integrity::Hash hash;
  bool contiguous = true;
  int result = ARCHIVE_OK;
  while (!job.cancelled && (result = archive_read_data_block(archive, &buffer, &bytes, &offset)) == ARCHIVE_OK) {
    if (offset < 0 || static_cast<std::uint64_t>(offset) > max_bytes || bytes > max_bytes - static_cast<std::uint64_t>(offset))
      return "Invalid archive data offset.";
    {
      std::lock_guard lock(job.mutex);
      if (bytes > max_bytes - job.snapshot.written) return "Archive exceeds the extraction size limit.";
      job.snapshot.written += bytes;
    }
    std::size_t written = 0;
    while (written < bytes && !job.cancelled) {
      const auto count = pwrite(fd, static_cast<const char*>(buffer) + written, bytes - written, offset + written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return std::strerror(errno);
      written += count;
    }
    if (contiguous && static_cast<std::uint64_t>(offset) == hashed) {
      hash.update(buffer, bytes);
      hashed += bytes;
    } else {
      contiguous = false;
    }
    extent = std::max(extent, static_cast<std::uint64_t>(offset) + bytes);
  }
  if (job.cancelled) return {};
  if (result != ARCHIVE_EOF) return archive_error(archive, "Incomplete archive data.");
  if (extent != static_cast<std::uint64_t>(size)) return "Extracted file size mismatch.";
  if (fsync(fd)) return std::strerror(errno);
  if (contiguous && hashed == extent) digest = hash.finish();
  else if (!file_hash(path, job, digest)) digest.clear();
  if (digest.size() != 64 && !job.cancelled) return "Cannot verify extracted file.";
  return {};
}
archive* open_archive(const Request& request, std::string& error) {
  auto* archive = archive_read_new();
  archive_read_support_filter_all(archive);
  archive_read_support_format_zip(archive);
  archive_read_support_format_rar(archive);
  archive_read_support_format_rar5(archive);
  archive_read_support_format_7zip(archive);
  archive_read_support_format_tar(archive);
  std::vector<const char*> files;
  for (const auto& path : request.sources) files.push_back(path.c_str());
  files.push_back(nullptr);
  if (archive_read_open_filenames(archive, files.data(), read_block_size) != ARCHIVE_OK)
    error = archive_error(archive, "Unsupported or incomplete archive.");
  return archive;
}
// Writes every entry under root and appends its digest line to the receipt.
std::string extract(Job& job, archive* archive, int root, const std::string& staging, std::string& receipt) {
  const auto& request = job.request;
  archive_entry* entry = nullptr;
  int result = ARCHIVE_OK;
  std::uint64_t declared_total = 0;
  while (!job.cancelled && (result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char* name = archive_entry_pathname(entry);
    const auto type = archive_entry_filetype(entry);
    const bool link = archive_entry_symlink(entry) || archive_entry_hardlink(entry);
    const bool reserved = name && std::string(name).find(".ps5-react-") != std::string::npos;
    if (!safe(name) || reserved || link || (type != AE_IFREG && type != AE_IFDIR)) return "Archive contains an unsafe path, link or special file.";
    const auto size = archive_entry_size(entry);
    if (size < 0 || static_cast<std::uint64_t>(size) > request.max_bytes - declared_total) return "Archive exceeds the extraction size limit.";
    declared_total += size;
    std::string path(name);
    while (path.ends_with('/')) path.pop_back();
    // ShadowMount's default scan reaches one subdirectory. Publish images at the set root.
    if (type == AE_IFREG && image(path)) {
      const auto slash = path.find_last_of('/');
      if (slash != std::string::npos) path = path.substr(slash + 1);
    }
    std::string leaf;
    const int parent = parent_fd(root, path, leaf);
    if (parent < 0) return std::strerror(errno);
    if (type == AE_IFDIR) {
      const bool failed = mkdirat(parent, leaf.c_str(), 0700) && errno != EEXIST;
      const std::string error = failed ? std::strerror(errno) : "";
      close(parent);
      if (failed) return error;
      continue;
    }
    const int fd = openat(parent, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    close(parent);
    if (fd < 0) return std::strerror(errno);
    std::string digest;
    const auto error = write_entry(job, archive, fd, size, staging + "/" + path, digest);
    close(fd);
    if (!error.empty() || job.cancelled) return error;
    receipt += digest + "|" + path + "\n";
    if (receipt.size() > max_metadata_bytes) return "Extraction receipt exceeds the metadata limit.";
    std::lock_guard lock(job.mutex);
    if (installable(path)) {
      if (job.snapshot.artifacts.size() == max_artifacts) return "Archive has too many installable artifacts.";
      job.snapshot.artifacts.push_back(path);
    }
    if (++job.snapshot.entries > max_entries) return "Archive has too many entries.";
  }
  if (!job.cancelled && result != ARCHIVE_EOF) return archive_error(archive, "Incomplete archive headers.");
  return {};
}
void remove_owned_staging(const std::string& staging, const struct stat& owned) {
  struct stat current;
  if (lstat(staging.c_str(), &current) || current.st_dev != owned.st_dev || current.st_ino != owned.st_ino) return;
  const int cleanup = open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (cleanup < 0) return;
  const bool cleared = clear_directory(cleanup);
  close(cleanup);
  if (cleared) rmdir(staging.c_str());
}
void run(const std::shared_ptr<Job>& pointer) {
  name_thread("archive");
  Job& job = *pointer;
  const auto& request = job.request;
  const std::string staging = request.destination + ".extracting";
  struct stat st;
  const auto identity = input_identity(request);
  if (identity.empty()) {
    fail(job, "Archive source is missing or not a regular file.");
    return;
  }
  if (!lstat(request.destination.c_str(), &st)) {
    if (S_ISDIR(st.st_mode) && recover(job, identity)) {
      std::lock_guard lock(job.mutex);
      job.snapshot.state = "completed";
    } else {
      fail(job, "Existing extraction has no matching, valid completion receipt; files preserved.");
    }
    return;
  }
  if (const auto refusal = preflight(request.sources); !refusal.empty()) {
    fail(job, refusal);
    return;
  }
  if (!lstat(staging.c_str(), &st)) {
    std::string owner;
    if (!S_ISDIR(st.st_mode) || !read_metadata(staging + "/" + stage_name, owner) || owner != "P5ST001:" + identity + "\n") {
      fail(job, "Staging belongs to another extraction; files preserved.");
      return;
    }
    if (!restart_staging(staging, st)) {
      fail(job, "Cannot restart owned extraction staging.");
      return;
    }
  }
  if (mkdir(staging.c_str(), 0700)) {
    fail(job, std::strerror(errno));
    return;
  }
  const int root = open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (root < 0) {
    fail(job, std::strerror(errno));
    return;
  }
  struct stat owned;
  if (fstat(root, &owned)) {
    fail(job, std::strerror(errno));
    close(root);
    rmdir(staging.c_str());
    return;
  }
  if (!write_metadata(root, stage_name, "P5ST001:" + identity + "\n") || fsync(root)) {
    close(root);
    fail(job, "Cannot record extraction staging ownership.");
    return;
  }
  std::string receipt = "P5AR001:" + identity + "\n";
  std::string error;
  auto* archive = open_archive(request, error);
  {
    std::lock_guard lock(job.mutex);
    job.snapshot.state = "extracting";
  }
  if (error.empty()) error = extract(job, archive, root, staging, receipt);
  archive_read_free(archive);
  if (error.empty() && !job.cancelled && !write_metadata(root, receipt_name, receipt)) error = "Cannot save extraction completion receipt.";
  if (error.empty() && !job.cancelled && fsync(root)) error = std::strerror(errno);
  close(root);
  if (job.cancelled || !error.empty()) {
    // Only this task created staging. Input parts and existing destinations are never removed.
    remove_owned_staging(staging, owned);
    fail(job, error.empty() ? "Extraction cancelled; downloaded parts are preserved." : error);
    return;
  }
  const bool still_owned = !lstat(staging.c_str(), &st) && st.st_dev == owned.st_dev && st.st_ino == owned.st_ino;
  const bool destination_taken = !lstat(request.destination.c_str(), &st);
  if (!still_owned || destination_taken || rename(staging.c_str(), request.destination.c_str())) {
    fail(job, "Cannot publish extraction; staging and inputs were preserved.");
    return;
  }
  std::lock_guard lock(job.mutex);
  job.snapshot.state = "completed";
}
}
std::uint32_t enqueue(Request request, std::string& error) {
  std::lock_guard lock(guard);
  if (active) {
    error = "An archive task is already running; poll its completion first.";
    return 0;
  }
  if (request.sources.empty() || request.sources.size() > 1024 || request.destination.empty() || !request.max_bytes) {
    error = "Invalid archive request.";
    return 0;
  }
  auto job = std::make_shared<Job>();
  job->request = std::move(request);
  job->snapshot.id = next_id++;
  job->snapshot.destination = job->request.destination;
  active = job;
  job->worker = std::thread(run, job);
  return job->snapshot.id;
}
void cancel(std::uint32_t id) {
  std::lock_guard lock(guard);
  if (active && active->snapshot.id == id) active->cancelled = true;
}
std::vector<Snapshot> poll() {
  std::lock_guard lock(guard);
  if (!active) return {};
  Snapshot snapshot;
  {
    std::lock_guard job_lock(active->mutex);
    snapshot = active->snapshot;
  }
  if (terminal(snapshot.state)) {
    active->worker.join();
    active.reset();
  }
  return {snapshot};
}
void stop() {
  std::lock_guard lock(guard);
  if (!active) return;
  active->cancelled = true;
  active->worker.join();
  active.reset();
}
}
