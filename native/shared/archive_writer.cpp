// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "archive_writer.hpp"
#include "archives.hpp"
#include "digest.hpp"
#include "thread_name.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

namespace archives {
namespace {
// Begin and end commands hold no ring bytes, and a sparse file's data commands may hold one byte each;
// this caps them for archives of many empty files or tiny scattered blocks.
constexpr std::size_t max_commands = 4096, max_closing = 64, readback_block = 128 * 1024;
// Writer threads only hash and copy; the read-back buffer is on the heap.
constexpr std::size_t thread_stack = 256 * 1024;
}
// Waits have no timeout (no timed wait has been proven on the console): every change a waiter needs is
// notified. The job's cancel flag is not, so a waiter sees it at the next notification, which always
// comes while the other side has queued work: commands are sealed before any wait for room.

Writer::~Writer() {
  if (!ring_) return;
  aborted_ = true;
  stop_threads();
}

bool Writer::start(std::size_t ring_bytes) {
  if (ring_bytes < 1024 * 1024) return false;
  ring_ = static_cast<unsigned char*>(std::malloc(ring_bytes));
  if (!ring_) return false;
  capacity_ = ring_bytes;
  chunk_ = std::min<std::size_t>(4 * 1024 * 1024, capacity_ / 4);
  if (!closer_.start([this] { close_loop(); }, thread_stack)) {
    std::free(ring_);
    ring_ = nullptr;
    return false;
  }
  if (!writer_.start([this] { write_loop(); }, thread_stack)) {
    {
      std::lock_guard lock(mutex_);
      closer_stopping_ = true;
    }
    close_work_.notify_all();
    closer_.join();
    std::free(ring_);
    ring_ = nullptr;
    return false;
  }
  return true;
}

void Writer::fail(const std::string& error) {
  std::lock_guard lock(mutex_);
  if (error_.empty()) error_ = error;
  failed_ = true;
  space_.notify_all();
  work_.notify_all();
}

// Empty on cancellation, as the inline path returns, so the job reports the cancel and not an error.
std::string Writer::error() {
  if (cancelled_) return {};
  std::lock_guard lock(mutex_);
  return error_.empty() && failed() ? "Extraction stopped." : error_;
}

bool Writer::push(Command command) {
  std::unique_lock lock(mutex_);
  if (!commands_.empty()) commands_.back().sealed = true;
  work_.notify_one();
  space_.wait(lock, [&] { return commands_.size() < max_commands || failed(); });
  if (failed()) {
    if (command.kind == Kind::begin) close(command.fd);
    return false;
  }
  commands_.push_back(std::move(command));
  work_.notify_one();
  return true;
}

bool Writer::begin(int fd, std::string path, std::string name) {
  Command command{Kind::begin};
  command.fd = fd;
  command.path = std::move(path);
  command.name = std::move(name);
  return push(std::move(command));
}

bool Writer::end() { return push(Command{Kind::end}); }

bool Writer::append(const void* bytes, std::size_t size, std::uint64_t offset) {
  const auto* source = static_cast<const unsigned char*>(bytes);
  while (size) {
    const std::size_t piece = std::min(size, chunk_);
    std::unique_lock lock(mutex_);
    if (failed()) return false;
    const std::size_t position = head_ % capacity_;
    Command* last = commands_.empty() ? nullptr : &commands_.back();
    // Merges into the queued write when these bytes follow it both in the file and in the ring.
    const bool merge = last && last->kind == Kind::data && !last->sealed && last->offset + last->length == offset
        && last->length + piece <= chunk_ && last->ring + last->length == position
        && position + piece <= capacity_ && head_ - tail_ + piece <= capacity_;
    if (merge) {
      last->busy = true;
      head_ += piece;
      lock.unlock();
      std::memcpy(ring_ + position, source, piece);
      lock.lock();
      last->length += piece;
      last->busy = false;
      if (last->length == chunk_) last->sealed = true;
      work_.notify_one();
    } else {
      const std::size_t pad = position + piece > capacity_ ? capacity_ - position : 0;
      if (last) last->sealed = true;
      work_.notify_one();
      space_.wait(lock, [&] { return (head_ - tail_ + pad + piece <= capacity_ && commands_.size() < max_commands) || failed(); });
      if (failed()) return false;
      Command command{Kind::data};
      command.ring = (head_ + pad) % capacity_;
      command.pad = pad;
      command.offset = offset;
      command.length = piece;
      command.busy = true;
      command.sealed = piece == chunk_;
      head_ += pad + piece;
      commands_.push_back(std::move(command));
      Command& queued = commands_.back();
      lock.unlock();
      std::memcpy(ring_ + queued.ring, source, piece);
      lock.lock();
      queued.busy = false;
      work_.notify_one();
    }
    source += piece;
    offset += piece;
    size -= piece;
  }
  return true;
}

void Writer::write_loop() {
  name_thread("archive-write");
  int fd = -1;
  std::string path, name;
  std::optional<integrity::Hash> hash;
  std::uint64_t hashed = 0, extent = 0;
  bool contiguous = true;
  while (true) {
    Command command;
    {
      std::unique_lock lock(mutex_);
      // A write still open to merging waits until the decoder seals it: a full chunk, the next file,
      // a wait for room, or the end.
      work_.wait(lock, [&] {
        if (commands_.empty()) return stopping_;
        const Command& front = commands_.front();
        return !front.busy && (front.sealed || stopping_ || failed());
      });
      if (commands_.empty()) {
        if (fd >= 0) close(fd);
        return;
      }
      command = std::move(commands_.front());
      commands_.pop_front();
      space_.notify_all();
    }
    const bool skip = failed();
    if (command.kind == Kind::begin) {
      if (fd >= 0) close(fd);
      fd = command.fd;
      path = std::move(command.path);
      name = std::move(command.name);
      hash.emplace();
      hashed = extent = 0;
      contiguous = true;
      if (skip) {
        close(fd);
        fd = -1;
      }
    } else if (command.kind == Kind::data) {
      const unsigned char* bytes = ring_ + command.ring;
      std::size_t written = 0;
      if (command.ring + command.length > capacity_) fail("Extraction write outside its buffer.");
      while (!skip && fd >= 0 && written < command.length && !failed()) {
        const auto count = pwrite(fd, bytes + written, command.length - written, command.offset + written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          fail(count < 0 ? std::strerror(errno) : "Short write.");
          break;
        }
        written += count;
      }
      if (!failed() && fd >= 0) {
        if (contiguous && command.offset == hashed) {
          hash->update(bytes, command.length);
          hashed += command.length;
        } else {
          contiguous = false;
        }
        extent = std::max<std::uint64_t>(extent, command.offset + command.length);
      }
      std::lock_guard lock(mutex_);
      tail_ += command.pad + command.length;
      space_.notify_all();
    } else if (fd >= 0) {
      std::string digest;
      if (!skip) {
        if (contiguous && hashed == extent) digest = hash->finish();
        else digest = readback(path);
      }
      if (!skip && digest.size() != 64) fail("Cannot verify extracted file.");
      if (!failed()) {
        receipt_ += digest + "|" + name + "\n";
        if (receipt_.size() > max_metadata_bytes) fail("Extraction receipt exceeds the metadata limit.");
      }
      std::unique_lock lock(mutex_);
      space_.wait(lock, [&] { return closing_.size() < max_closing || failed(); });
      closing_.push_back(fd);
      fd = -1;
      close_work_.notify_one();
    }
  }
}

std::string Writer::readback(const std::string& file) {
  const int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return {};
  auto* buffer = static_cast<char*>(std::malloc(readback_block));
  integrity::Hash hash;
  bool ok = buffer;
  while (ok && !failed()) {
    const auto bytes = read(fd, buffer, readback_block);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) {
      ok = !bytes;
      break;
    }
    ok = hash.update(buffer, bytes);
  }
  std::free(buffer);
  close(fd);
  return ok && !failed() ? hash.finish() : std::string();
}

// fsync before the receipt: inputs are deleted once extraction completes, so the files must be on disk.
void Writer::close_loop() {
  name_thread("archive-close");
  while (true) {
    int fd;
    {
      std::unique_lock lock(mutex_);
      close_work_.wait(lock, [&] { return !closing_.empty() || closer_stopping_; });
      if (closing_.empty()) return;
      fd = closing_.front();
      closing_.pop_front();
      space_.notify_all();
    }
    if (!failed() && fsync(fd)) fail(std::strerror(errno));
    if (close(fd) && !failed() && errno != EINTR) fail(std::strerror(errno));
  }
}

void Writer::stop_threads() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    if (!commands_.empty()) commands_.back().sealed = true;
  }
  work_.notify_all();
  writer_.join();
  {
    std::lock_guard lock(mutex_);
    closer_stopping_ = true;
  }
  close_work_.notify_all();
  closer_.join();
  std::free(ring_);
  ring_ = nullptr;
}

std::string Writer::finish(std::string& receipt) {
  stop_threads();
  if (cancelled_) return {};
  if (failed_ || aborted_) return error_.empty() ? "Extraction stopped." : error_;
  receipt += receipt_;
  return {};
}
}
