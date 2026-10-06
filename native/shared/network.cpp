// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"
#ifdef PROSPERO
#include "app_config.hpp"
#endif
#if !defined(PROSPERO) || PS5_REACT_NETWORKING
#include <curl/curl.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>

namespace network {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t block_size = 256 * 1024, block_count = 32;
constexpr unsigned max_connections = 16, max_jobs = 8, max_ranges = 65536;
static_assert(sizeof(off_t) >= 8, "Large downloads require 64-bit file offsets");
constexpr std::uint64_t safe_integer = 9007199254740991ULL;

// Used for optional final-file verification and checkpoint identity, off the JS thread.
class Hash {
public:
  explicit Hash(bool sha1 = false) : sha1_(sha1) {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    valid_ = (sha1_ ? CC_SHA1_Init(&sha1_context_) : CC_SHA256_Init(&context_)) == 1;
#else
    context_ = EVP_MD_CTX_new();
    valid_ = context_ && EVP_DigestInit_ex(context_, sha1_ ? EVP_sha1() : EVP_sha256(), nullptr) == 1;
#endif
  }
  ~Hash() {
#ifndef __APPLE__
    EVP_MD_CTX_free(context_);
#endif
  }
  bool update(const void* bytes, std::size_t size) {
#ifdef __APPLE__
    valid_ = valid_ && (sha1_ ? CC_SHA1_Update(&sha1_context_, bytes, static_cast<CC_LONG>(size)) :
                      CC_SHA256_Update(&context_, bytes, static_cast<CC_LONG>(size))) == 1;
#else
    valid_ = valid_ && EVP_DigestUpdate(context_, bytes, size) == 1;
#endif
    return valid_;
  }
  std::string finish() {
    unsigned char digest[32];
    const unsigned wanted = sha1_ ? 20 : 32;
#ifdef __APPLE__
    if (!valid_ || (sha1_ ? CC_SHA1_Final(digest, &sha1_context_) : CC_SHA256_Final(digest, &context_)) != 1) return {};
#pragma clang diagnostic pop
#else
    unsigned size = 0;
    if (!valid_ || EVP_DigestFinal_ex(context_, digest, &size) != 1 || size != wanted) return {};
#endif
    constexpr char digits[] = "0123456789abcdef";
    std::string out(wanted*2, '0');
    for (unsigned i = 0; i < wanted; ++i) {
      out[2*i] = digits[digest[i] >> 4]; out[2*i+1] = digits[digest[i] & 15];
    }
    return out;
  }
private:
#ifdef __APPLE__
  CC_SHA256_CTX context_{};
  CC_SHA1_CTX sha1_context_{};
#else
  EVP_MD_CTX* context_ = nullptr;
#endif
  bool valid_ = false;
  bool sha1_ = false;
};

struct Source {
  Piece piece;
  std::string etag;
  bool ranged = false;
};
struct Segment {
  std::uint64_t begin = 0, length = 0;
  unsigned source = 0;
};
struct Job {
  explicit Job(Request value, std::uint32_t id) : request(std::move(value)) {
    snapshot.id = id; snapshot.destination = request.destination;
  }
  ~Job() { if (root_fd >= 0) close(root_fd); }
  Request request;
  Snapshot snapshot;
  std::mutex mutex;
  std::atomic<bool> cancelled{false}, failed{false}, finalized{false};
  std::atomic<unsigned> pending{0};
  int fd = -1;
  int root_fd = -1;
  struct stat root_stat{};
  Clock::time_point storage_checked{};
  std::string etag, identity, response, receipt_identity, verified_digest;
  std::vector<unsigned char> completed;
  std::vector<Source> sources;
  std::vector<Segment> segments;
  std::uint64_t total = 0, committed = 0;
  unsigned retries = 0;
  bool ranged = false, known = false, checkpoint_ready = false, recovering = false;
  void fail(const std::string& reason) {
    std::lock_guard lock(mutex);
    if (!failed.exchange(true)) snapshot.error = reason;
  }
};

const char* terminal_state(const Job& job) {
  if (job.failed) return "failed";
  if (job.cancelled) return "cancelled";
  return "completed";
}

struct Block { char* data = nullptr; };
class Service;
struct Transfer {
  Service* owner = nullptr;
  CURL* curl = nullptr;
  curl_slist* headers = nullptr;
  std::shared_ptr<Job> job;
  Block* block = nullptr;
  std::size_t fill = 0, header_bytes = 0;
  std::uint64_t begin = 0, length = 0, accepted = 0, block_offset = 0;
  std::atomic<std::uint64_t> written{0};
  std::atomic<unsigned> pending{0};
  unsigned piece = 0, source = 0, retry_after = 0;
  long status = 0;
  std::string etag, encoding, error;
  std::uint64_t content_length = 0;
  bool length_known = false;
  std::uint64_t range_begin = 0, range_end = 0, range_total = 0;
  bool content_range = false, probe = false, paused = false, active = false, draining = false;
  CURLcode result = CURLE_OK;
};

bool valid_file_response(const Transfer& t) {
  const Job& job = *t.job;
  if (!t.encoding.empty() && t.encoding != "identity") return false;
  if (!job.sources.empty()) {
    const auto& source = job.sources[t.source];
    if (!source.ranged) return t.status == 200;
    const auto begin = t.begin-source.piece.offset;
    return t.status == 206 && t.content_range && t.range_begin == begin &&
      t.range_end == begin+t.length-1 && t.range_total == source.piece.size &&
      (source.etag.empty() || t.etag == source.etag);
  }
  if (!job.ranged) return t.status == 200;
  return t.status == 206 && t.content_range && t.range_begin == t.begin &&
         t.range_end == t.begin+t.length-1 && t.range_total == job.total &&
         (job.etag.empty() || t.etag == job.etag);
}

bool retryable(const Transfer& t) {
  return (t.error.empty() && (t.result == CURLE_RECV_ERROR || t.result == CURLE_SEND_ERROR ||
          t.result == CURLE_OPERATION_TIMEDOUT || t.result == CURLE_PARTIAL_FILE ||
          t.result == CURLE_COULDNT_CONNECT)) ||
         t.status == 429 || t.status == 502 || t.status == 503 || t.status == 504;
}

struct Write {
  std::shared_ptr<Job> job;
  Transfer* transfer = nullptr;
  Block* block = nullptr;
  std::uint64_t offset = 0;
  std::size_t size = 0;
  std::vector<unsigned char> checkpoint;
  bool finish = false;
};

struct Checkpoint {
  char magic[8] = {'P','5','R','D','L','0','0','1'};
  std::uint64_t total = 0, range_bytes = 0;
  std::uint32_t count = 0;
  char identity[65] = {};
};

bool write_all(int fd, const void* bytes, std::size_t size, std::uint64_t offset) {
  const char* data = static_cast<const char*>(bytes);
  while (size) {
    const ssize_t n = pwrite(fd, data, size, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n; size -= static_cast<std::size_t>(n); offset += static_cast<std::uint64_t>(n);
  }
  return true;
}

bool read_all(int fd, void* bytes, std::size_t size) {
  char* data = static_cast<char*>(bytes);
  while (size) {
    const ssize_t n = read(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n; size -= static_cast<std::size_t>(n);
  }
  return true;
}

std::string system_error(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}

bool storage_matches(Job& job) {
  if (job.root_fd < 0) return true;
  const auto now = Clock::now();
  if (job.storage_checked != Clock::time_point{} && now-job.storage_checked < std::chrono::seconds(1)) return true;
  job.storage_checked = now;
  struct stat current;
  if (stat(job.request.storage_root.c_str(), &current) != 0 || current.st_dev != job.root_stat.st_dev ||
      current.st_ino != job.root_stat.st_ino) {
    job.fail("storage disconnected or changed; reconnect the original drive before resuming"); return false;
  }
  return true;
}

bool sync_parent(const std::string& path) {
  const auto parent = path.substr(0, path.find_last_of('/'));
  const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0) return false;
  const int result = fsync(fd), saved = errno;
  close(fd);
  // Some filesystems do not implement directory fsync.
  return result == 0 || saved == EINVAL || saved == ENOTSUP;
}

struct CompletionReceipt {
  char magic[8] = {'P', 'S', '5', 'D', 'O', 'N', 'E', '1'};
  char identity[65]{}, sha256[65]{};
  std::uint64_t total = 0, device = 0, inode = 0;
};

std::string receipt_identity(const Job& job) {
  Hash hash;
  const auto add = [&](const std::string& value) {
    return hash.update(value.data(), value.size()) && hash.update("\0", 1);
  };
  if (!add(job.request.url) || !add(job.request.sha256) || !add(std::to_string(job.request.expected_bytes)) ||
      !add(std::to_string(job.root_stat.st_dev)) || !add(std::to_string(job.root_stat.st_ino))) return {};
  for (const auto& piece : job.request.pieces)
    if (!add(piece.url) || !add(piece.sha1) || !add(std::to_string(piece.offset)) || !add(std::to_string(piece.size))) return {};
  return hash.finish();
}

bool save_receipt(Job& job) {
  struct stat file{};
  if (fstat(job.fd, &file) != 0) { job.fail(system_error("inspect completed file")); return false; }
  CompletionReceipt receipt;
  std::memcpy(receipt.identity, job.receipt_identity.c_str(), 64);
  std::memcpy(receipt.sha256, job.verified_digest.c_str(), 64);
  receipt.total = job.total; receipt.device = file.st_dev; receipt.inode = file.st_ino;
  const auto path = job.request.destination+".complete", temporary = path+".tmp";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) { job.fail(system_error("open completion receipt")); return false; }
  bool ok = write_all(fd, &receipt, sizeof receipt, 0) && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0 && sync_parent(path);
  if (!ok) job.fail(system_error("save completion receipt"));
  return ok;
}

bool recover_file(Job& job) {
  CompletionReceipt receipt, expected;
  const int meta = open((job.request.destination+".complete").c_str(), O_RDONLY | O_NOFOLLOW);
  const bool valid = meta >= 0 && read_all(meta, &receipt, sizeof receipt) &&
    !std::memcmp(receipt.magic, expected.magic, sizeof receipt.magic) &&
    !std::memcmp(receipt.identity, job.receipt_identity.c_str(), 65) && receipt.sha256[64] == '\0' &&
    std::all_of(receipt.sha256, receipt.sha256+64, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  if (meta >= 0) close(meta);
  struct stat file{};
  job.fd = open(job.request.destination.c_str(), O_RDONLY | O_NOFOLLOW);
  if (!valid || job.fd < 0 || fstat(job.fd, &file) != 0 || !S_ISREG(file.st_mode) || file.st_size < 0 ||
      static_cast<std::uint64_t>(file.st_size) != receipt.total ||
      static_cast<std::uint64_t>(file.st_dev) != receipt.device || static_cast<std::uint64_t>(file.st_ino) != receipt.inode ||
      (!job.request.sha256.empty() && job.request.sha256 != receipt.sha256) ||
      (job.request.expected_bytes && receipt.total != job.request.expected_bytes)) {
    if (job.fd >= 0) close(job.fd);
    job.fd = -1;
    job.fail("existing destination has no matching completion receipt; preserve it and inspect it"); return false;
  }
  job.request.sha256 = receipt.sha256;
  job.total = job.committed = receipt.total; job.known = job.recovering = true;
  return true;
}

bool publish_file(Job& job) {
  // Persist proof of verification before the final name can become visible.
  if (job.request.recover_completed && !save_receipt(job)) return false;
  const std::string partial = job.request.destination+".part";
  if (link(partial.c_str(), job.request.destination.c_str()) == 0) {
    if (unlink(partial.c_str()) != 0) { job.fail(system_error("remove published partial")); return false; }
  } else {
    if (errno != EPERM && errno != ENOTSUP && errno != ENOSYS) {
      job.fail(system_error("publish completed file without overwriting")); return false;
    }
    // exFAT cannot create hard links. Reserve the destination exclusively before rename.
    const int reserved = open(job.request.destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (reserved < 0) { job.fail(system_error("reserve completed filename")); return false; }
    close(reserved);
    if (rename(partial.c_str(), job.request.destination.c_str()) != 0) {
      const int saved = errno; unlink(job.request.destination.c_str()); errno = saved;
      job.fail(system_error("publish completed file")); return false;
    }
  }
  unlink((job.request.destination+".resume").c_str());
  if (!sync_parent(job.request.destination)) { job.fail(system_error("sync completed directory")); return false; }
  return true;
}

bool checkpoint(Job& job, const std::vector<unsigned char>& completed) {
  if (!job.ranged) return true;
  if (fsync(job.fd) != 0) { job.fail(system_error("sync partial file")); return false; }
  Checkpoint header;
  header.total = job.total; header.range_bytes = job.request.range_bytes;
  header.count = static_cast<std::uint32_t>(completed.size());
  std::memcpy(header.identity, job.identity.c_str(), 64);
  const std::string path = job.request.destination + ".resume", temporary = path + ".tmp";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) { job.fail(system_error("open checkpoint")); return false; }
  bool ok = write_all(fd, &header, sizeof header, 0) &&
            write_all(fd, completed.data(), completed.size(), sizeof header) && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
  if (ok) ok = sync_parent(path);
  if (!ok) job.fail(system_error("save checkpoint"));
  return ok;
}

bool verify_file(Job& job) {
  const bool piece_hashes = std::any_of(job.sources.begin(), job.sources.end(),
    [](const Source& source) { return !source.piece.sha1.empty(); });
  if (job.request.sha256.empty() && !piece_hashes && !job.request.recover_completed) return true;
  { std::lock_guard lock(job.mutex); job.snapshot.state = "verifying"; }
  Hash hash;
  auto buffer = std::make_unique<char[]>(block_size);
  std::size_t source = 0;
  auto piece_hash = std::make_unique<Hash>(true);
  for (std::uint64_t offset = 0; offset < job.total;) {
    if (job.cancelled) return false;
    const auto end = job.sources.empty() ? job.total :
      job.sources[source].piece.offset+job.sources[source].piece.size;
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(block_size, end-offset));
    const ssize_t n = pread(job.fd, buffer.get(), wanted, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || !hash.update(buffer.get(), static_cast<std::size_t>(n))) {
      job.fail("read/hash verification failed"); return false;
    }
    offset += static_cast<std::uint64_t>(n);
    if (!job.sources.empty()) {
      if (!piece_hash->update(buffer.get(), static_cast<std::size_t>(n))) {
        job.fail("SHA-1 verification failed"); return false;
      }
      if (offset == end) {
        const auto& expected = job.sources[source].piece.sha1;
        if (!expected.empty() && piece_hash->finish() != expected) {
          job.fail("SHA-1 mismatch in piece " + std::to_string(source)); return false;
        }
        ++source;
        piece_hash = std::make_unique<Hash>(true);
      }
    }
  }
  job.verified_digest = hash.finish();
  if (job.verified_digest.empty()) { job.fail("SHA-256 verification failed"); return false; }
  if (!job.request.sha256.empty() && job.verified_digest != job.request.sha256) { job.fail("SHA-256 mismatch"); return false; }
  return true;
}

class Service {
public:
  bool start();
  void stop();
  std::uint32_t enqueue(Request request, std::string& error);
  void cancel(std::uint32_t id);
  std::vector<Snapshot> poll();
private:
  std::mutex mutex_, disk_mutex_;
  std::condition_variable wake_, disk_wake_;
  std::deque<std::shared_ptr<Job>> jobs_, waiting_;
  std::deque<Write> writes_;
  std::vector<Block*> free_;
  std::array<Block, block_count> blocks_{};
  std::array<Transfer, max_connections> transfers_{};
  CURLM* multi_ = nullptr;
  std::atomic<bool> stopping_{false}, writer_stopping_{false};
  pthread_t network_thread_{}, writer_thread_{};
  bool running_ = false, network_started_ = false, writer_started_ = false, platform_started_ = false, curl_started_ = false;
  std::uint32_t next_id_ = 1;
  std::string error_;
  Block* take_block() {
    std::lock_guard lock(disk_mutex_);
    if (free_.empty()) return nullptr;
    Block* out = free_.back(); free_.pop_back(); return out;
  }
  void release(Block* block) {
    std::lock_guard lock(disk_mutex_); free_.push_back(block);
  }
  void flush(Transfer& t) {
    if (!t.block) return;
    if (!t.fill) { release(t.block); t.block = nullptr; return; }
    Write command; command.job = t.job; command.transfer = &t; command.block = t.block;
    command.offset = t.block_offset; command.size = t.fill;
    ++t.pending; ++t.job->pending;
    { std::lock_guard lock(disk_mutex_); writes_.push_back(std::move(command)); }
    t.block = nullptr; t.fill = 0; disk_wake_.notify_one();
  }
  void queue_checkpoint(const std::shared_ptr<Job>& job, bool finish) {
    Write command; command.job = job; command.checkpoint = job->completed; command.finish = finish;
    ++job->pending;
    { std::lock_guard lock(disk_mutex_); writes_.push_back(std::move(command)); }
    disk_wake_.notify_one();
  }
  void network_loop();
  void writer_loop();
  void run_job(const std::shared_ptr<Job>& job);
  bool prepare(Job& job);
  bool probe_source(Job& job, unsigned source);
  CURLcode perform_probe(Transfer& probe);
  bool configure(Transfer& t);
  void finish_transfer(Transfer& t);
  void snapshot(Job& job, unsigned allowed, Clock::time_point now, Clock::time_point& previous,
                std::uint64_t& previous_written);
  static std::size_t body(char* bytes, std::size_t size, std::size_t count, void* opaque);
  static std::size_t header(char* bytes, std::size_t size, std::size_t count, void* opaque);
  static int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(opaque)->job->cancelled.load() ? 1 : 0;
  }
};
Service service;

bool spawn(pthread_t& thread, void* (*entry)(void*), void* data) {
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  const int stack = pthread_attr_setstacksize(&attributes, 1024 * 1024);
  const int result = stack ? stack : pthread_create(&thread, &attributes, entry, data);
  pthread_attr_destroy(&attributes);
  return result == 0;
}

bool Service::start() {
  if (running_) return true;
  stopping_ = false; writer_stopping_ = false;
  platform_started_ = platform_start(error_);
  if (!platform_started_) return false;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { error_ = "curl initialization failed"; stop(); return false; }
  curl_started_ = true;
  multi_ = curl_multi_init();
  if (!multi_) { error_ = "curl multi initialization failed"; stop(); return false; }
  for (Block& block : blocks_) {
    block.data = static_cast<char*>(std::malloc(block_size));
    if (!block.data) { error_ = "download buffer allocation failed"; stop(); return false; }
    free_.push_back(&block);
  }
  if (curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(max_connections)) != CURLM_OK ||
      curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(max_connections)) != CURLM_OK) {
    error_ = "curl connection limit configuration failed"; stop(); return false;
  }
  for (Transfer& t : transfers_) {
    t.owner = this;
    t.curl = curl_easy_init();
    if (!t.curl) { error_ = "curl handle allocation failed"; stop(); return false; }
  }
  writer_started_ = spawn(writer_thread_, +[](void* p)->void* { static_cast<Service*>(p)->writer_loop(); return nullptr; }, this);
  network_started_ = writer_started_ && spawn(network_thread_, +[](void* p)->void* { static_cast<Service*>(p)->network_loop(); return nullptr; }, this);
  if (!network_started_) { error_ = "download worker creation failed"; stop(); return false; }
  running_ = true; return true;
}

void Service::stop() {
  stopping_ = true;
  { std::lock_guard lock(mutex_); for (const auto& job : jobs_) job->cancelled = true; }
  wake_.notify_all();
  if (multi_) curl_multi_wakeup(multi_);
  if (network_started_) pthread_join(network_thread_, nullptr);
  network_started_ = false;
  writer_stopping_ = true;
  disk_wake_.notify_all();
  if (writer_started_) pthread_join(writer_thread_, nullptr);
  writer_started_ = false;
  for (Transfer& t : transfers_) {
    if (t.headers) curl_slist_free_all(t.headers);
    if (t.curl) curl_easy_cleanup(t.curl);
    t.headers = nullptr; t.curl = nullptr; t.job.reset();
  }
  if (multi_) curl_multi_cleanup(multi_);
  multi_ = nullptr;
  for (Block& block : blocks_) { std::free(block.data); block.data = nullptr; }
  free_.clear(); jobs_.clear(); waiting_.clear(); writes_.clear();
  if (curl_started_) curl_global_cleanup();
  curl_started_ = false;
  if (platform_started_) platform_stop();
  platform_started_ = false; running_ = false;
}

std::uint32_t Service::enqueue(Request request, std::string& error) {
  if (!running_) { error = error_.empty() ? "networking is not initialized" : error_; return 0; }
  if (!request.pieces.empty()) {
    std::uint64_t offset = 0;
    bool valid = request.pieces.size() <= 1024 && !request.destination.empty();
    for (const auto& piece : request.pieces) {
      valid = valid && piece.offset == offset && piece.size > 0 && piece.size <= safe_integer-offset;
      if (!valid) break;
      offset += piece.size;
    }
    if (!valid || offset != request.expected_bytes) {
      error = "manifest pieces must cover expectedBytes exactly without gaps or overlaps"; return 0;
    }
  }
  std::lock_guard lock(mutex_);
  if (jobs_.size() >= max_jobs) { error = "download/request queue is full (8 tasks); poll completed tasks"; return 0; }
  for (const auto& job : jobs_) if (!request.destination.empty() && job->request.destination == request.destination) {
    error = "destination already belongs to a queued task"; return 0;
  }
  auto job = std::make_shared<Job>(std::move(request), next_id_++);
  jobs_.push_back(job); waiting_.push_back(job); wake_.notify_one();
  return job->snapshot.id;
}

void Service::cancel(std::uint32_t id) {
  std::lock_guard lock(mutex_);
  for (const auto& job : jobs_) if (job->snapshot.id == id) job->cancelled = true;
  if (multi_) curl_multi_wakeup(multi_);
}

std::vector<Snapshot> Service::poll() {
  std::vector<Snapshot> out;
  std::lock_guard lock(mutex_);
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    const auto& job = *it;
    { std::lock_guard job_lock(job->mutex); out.push_back(job->snapshot); }
    if (job->finalized) it = jobs_.erase(it); else ++it;
  }
  return out;
}

bool decimal(std::string_view text, std::uint64_t& value) {
  const auto result = std::from_chars(text.data(), text.data()+text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data()+text.size();
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
  return text;
}

std::size_t Service::header(char* data, std::size_t size, std::size_t count, void* opaque) {
  auto& t = *static_cast<Transfer*>(opaque);
  if (size && count > 65536/size) return 0;
  const auto length = size*count;
  if (length > 65536-t.header_bytes) { t.error = "response headers exceed 64 KiB"; return 0; }
  t.header_bytes += length;
  std::string_view line(data, length);
  if (line.starts_with("HTTP/")) {
    t.etag.clear(); t.encoding.clear(); t.content_range = false; t.length_known = false;
    const auto space = line.find(' ');
    if (space != std::string_view::npos && line.size() >= space+4) {
      std::uint64_t status = 0;
      if (decimal(line.substr(space+1, 3), status)) t.status = static_cast<long>(status);
    }
    return length;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) return length;
  std::string name(line.substr(0, colon));
  for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
  const auto value = trim(line.substr(colon+1));
  if (name == "etag") t.etag.assign(value);
  if (name == "content-encoding") t.encoding.assign(value);
  if (name == "content-length") t.length_known = decimal(value, t.content_length);
  if (name == "retry-after") {
    std::uint64_t seconds = 0;
    if (decimal(value, seconds)) t.retry_after = static_cast<unsigned>(std::min<std::uint64_t>(3600, seconds));
  }
  if (name == "content-range" && value.starts_with("bytes ")) {
    const auto dash = value.find('-'), slash = value.find('/');
    t.content_range = dash != std::string_view::npos && slash != std::string_view::npos && dash >= 6 && dash < slash &&
      decimal(value.substr(6, dash-6), t.range_begin) &&
      decimal(value.substr(dash+1, slash-dash-1), t.range_end) &&
      decimal(value.substr(slash+1), t.range_total) &&
      t.range_begin <= t.range_end && t.range_end < t.range_total;
  }
  return length;
}

std::size_t Service::body(char* data, std::size_t size, std::size_t count, void* opaque) {
  auto& t = *static_cast<Transfer*>(opaque);
  if (size && count > block_size/size) { t.error = "unexpected callback block size"; return 0; }
  const auto length = size*count;
  Job& job = *t.job;
  if (job.cancelled || job.failed) return 0;
  if (t.probe) {
    if (t.status == 200) return 0; // Range ignored: do not consume a potentially huge probe body.
    if (length > 1-t.accepted) { t.error = "invalid range probe body"; return 0; }
    t.accepted += length; return length;
  }
  if (!job.request.destination.empty()) {
    if (!valid_file_response(t)) {
      t.error = "server changed the resource or returned an invalid range/encoding/status"; return 0;
    }
    const std::uint64_t limit = job.ranged ? t.length : (job.known ? job.total : safe_integer);
    if (t.accepted > limit || length > limit-t.accepted) { t.error = "response exceeds expected size"; return 0; }
    if (t.block && block_size-t.fill < length) t.owner->flush(t);
    if (!t.block) {
      t.block = t.owner->take_block();
      if (!t.block) { t.paused = true; return CURL_WRITEFUNC_PAUSE; }
      t.block_offset = t.begin+t.accepted;
    }
    std::memcpy(t.block->data+t.fill, data, length);
    t.fill += length; t.accepted += length;
    if (t.fill == block_size) t.owner->flush(t);
  } else {
    if (length > job.request.max_bytes-t.accepted) { t.error = "response exceeds maxBytes"; return 0; }
    job.response.append(data, length); t.accepted += length;
  }
  return length;
}

bool Service::configure(Transfer& t) {
  curl_easy_reset(t.curl);
  if (t.headers) curl_slist_free_all(t.headers);
  t.headers = nullptr; t.status = 0; t.header_bytes = 0;
  t.etag.clear(); t.encoding.clear(); t.error.clear();
  t.content_range = false; t.length_known = false; t.paused = false; t.retry_after = 0;
  Job& job = *t.job;
  bool ok = true;
  const auto set = [&](CURLoption option, auto value) {
    if (curl_easy_setopt(t.curl, option, value) != CURLE_OK) ok = false;
  };
  const auto add = [&](const std::string& text) {
    curl_slist* list = curl_slist_append(t.headers, text.c_str());
    if (!list) ok = false; else t.headers = list;
  };
  for (const auto& text : job.request.headers) add(text);
  const Source* source = job.sources.empty() ? nullptr : &job.sources[t.source];
  const auto& url = source ? source->piece.url : job.request.url;
  const auto& etag = source ? source->etag : job.etag;
  const bool ranged = source ? source->ranged : job.ranged;
  if (ranged && !t.probe && !etag.empty()) add("If-Range: " + etag);
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_PROTOCOLS_STR, "http,https");
  set(CURLOPT_REDIR_PROTOCOLS_STR, url.starts_with("https:") ? "https" : "http,https");
  // Custom application headers must never be forwarded to an unrelated redirect origin.
  set(CURLOPT_FOLLOWLOCATION, job.request.headers.empty() ? 1L : 0L);
  set(CURLOPT_MAXREDIRS, 5L);
  set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
  set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
  if (const char* path = ca_path()) set(CURLOPT_CAINFO, path);
  set(CURLOPT_BUFFERSIZE, 256L*1024);
  set(CURLOPT_ACCEPT_ENCODING, "identity");
  set(CURLOPT_USERAGENT, "PS5React/0.1");
  set(CURLOPT_CONNECTTIMEOUT, 10L);
  set(CURLOPT_LOW_SPEED_LIMIT, 1L); set(CURLOPT_LOW_SPEED_TIME, 30L);
  set(CURLOPT_TCP_KEEPALIVE, 1L);
  set(CURLOPT_HTTPHEADER, t.headers);
  set(CURLOPT_WRITEFUNCTION, body); set(CURLOPT_WRITEDATA, &t);
  set(CURLOPT_HEADERFUNCTION, header); set(CURLOPT_HEADERDATA, &t);
  set(CURLOPT_XFERINFOFUNCTION, progress); set(CURLOPT_XFERINFODATA, &t);
  set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_PRIVATE, &t);
#ifdef PROSPERO
  set(CURLOPT_IPRESOLVE, static_cast<long>(CURL_IPRESOLVE_V4));
  set(CURLOPT_SOCKOPTFUNCTION, +[](void*, curl_socket_t fd, curlsocktype)->int {
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, 0x1200, &enabled, sizeof enabled) == 0 ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
  });
#endif
  if (t.probe) set(CURLOPT_RANGE, "0-0");
  else if (ranged) {
    const auto begin = t.begin-(source ? source->piece.offset : 0);
    const std::string range = std::to_string(begin)+"-"+std::to_string(begin+t.length-1);
    set(CURLOPT_RANGE, range.c_str());
  } else if (job.request.destination.empty()) {
    set(CURLOPT_TIMEOUT, 30L);
    set(CURLOPT_CUSTOMREQUEST, job.request.method.c_str());
    if (job.request.method == "HEAD") set(CURLOPT_NOBODY, 1L);
    if (!job.request.body.empty() || job.request.method == "POST") {
      set(CURLOPT_POSTFIELDS, job.request.body.c_str());
      set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(job.request.body.size()));
    }
  }
  if (!ok) t.error = "could not configure the HTTP transport";
  return ok;
}

CURLcode Service::perform_probe(Transfer& probe) {
  // Probe through the same multi handle so its TCP/TLS connection can be reused.
  CURLcode result = CURLE_FAILED_INIT;
  if (curl_multi_add_handle(multi_, probe.curl) != CURLM_OK) {
    probe.error = "could not schedule range probe"; return CURLE_FAILED_INIT;
  }
  bool done = false;
  while (!done && !probe.job->cancelled) {
    int running = 0;
    const auto state = curl_multi_perform(multi_, &running);
    if (state != CURLM_OK) { probe.error = curl_multi_strerror(state); break; }
    int left = 0;
    while (CURLMsg* message = curl_multi_info_read(multi_, &left)) {
      if (message->msg == CURLMSG_DONE && message->easy_handle == probe.curl) {
        result = message->data.result; done = true;
      }
    }
    if (!done) curl_multi_poll(multi_, nullptr, 0, 50, nullptr);
  }
  curl_multi_remove_handle(multi_, probe.curl);
  return probe.job->cancelled ? CURLE_ABORTED_BY_CALLBACK : result;
}

bool Service::probe_source(Job& job, unsigned source_index) {
  Transfer& probe = transfers_[0];
  probe.probe = true; probe.source = source_index;
  CURLcode result = CURLE_FAILED_INIT;
  for (unsigned attempt = 1; attempt <= 4; ++attempt) {
    probe.accepted = 0;
    if (!configure(probe)) { job.fail(probe.error); return false; }
    result = perform_probe(probe); probe.result = result;
    if (job.cancelled) return false;
    if (attempt == 4 || !retryable(probe)) break;
    ++job.retries;
    const auto retry_at = Clock::now()+std::chrono::milliseconds(std::max(probe.retry_after*1000u, 250u << attempt));
    while (!job.cancelled && Clock::now() < retry_at) curl_multi_poll(multi_, nullptr, 0, 50, nullptr);
  }
  if (job.cancelled) return false;
  if (probe.status == 200 && result == CURLE_WRITE_ERROR && probe.error.empty()) {
    job.known = probe.length_known; job.total = probe.content_length;
  } else if (result == CURLE_OK && probe.status == 206 && probe.content_range &&
             probe.range_begin == 0 && probe.range_end == 0 && probe.accepted == 1) {
    job.known = true; job.total = probe.range_total;
    if (!probe.etag.empty() && !probe.etag.starts_with("W/")) job.etag = probe.etag;
    job.ranged = !job.etag.empty() || !job.request.sha256.empty();
  } else if (result == CURLE_OK && probe.status == 200 && probe.length_known && probe.content_length == 0) {
    job.known = true; job.total = 0;
  } else {
    job.fail(probe.error.empty() ? "range probe failed (HTTP " + std::to_string(probe.status) + ", " + curl_easy_strerror(result) + ")" : probe.error);
    return false;
  }
  if ((!probe.encoding.empty() && probe.encoding != "identity") || job.total > safe_integer) {
    job.fail("unsupported content encoding or file size"); return false;
  }
  if (!job.sources.empty()) {
    auto& source = job.sources[source_index];
    if (!job.known || job.total != source.piece.size) {
      job.fail("manifest piece size does not match the server: " + std::to_string(source_index)); return false;
    }
    source.etag = job.etag;
    const bool protected_identity = !source.etag.empty() || !source.piece.sha1.empty() || !job.request.sha256.empty();
    if (!protected_identity) {
      job.fail("manifest pieces require a strong ETag, SHA-1, or whole-file SHA-256"); return false;
    }
    source.ranged = probe.status == 206;
  }
  return true;
}

bool Service::prepare(Job& job) {
  if (!job.request.storage_root.empty()) {
    job.root_fd = open(job.request.storage_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (job.root_fd < 0 || fstat(job.root_fd, &job.root_stat) != 0 || !storage_matches(job)) {
      job.fail("cannot open original storage root"); return false;
    }
  }
  if (job.request.recover_completed) {
    job.receipt_identity = receipt_identity(job);
    if (job.receipt_identity.empty()) { job.fail("completion identity hash failed"); return false; }
    struct stat existing{};
    if (lstat(job.request.destination.c_str(), &existing) == 0) return recover_file(job);
    if (errno != ENOENT) { job.fail(system_error("inspect destination")); return false; }
  }
  if (job.request.pieces.empty()) {
    if (!probe_source(job, 0)) return false;
    if (job.request.expected_bytes && (!job.known || job.total != job.request.expected_bytes)) {
      job.fail("expectedBytes does not match the server file size"); return false;
    }
  } else {
    for (const auto& piece : job.request.pieces) job.sources.push_back({piece, {}, false});
    for (unsigned i = 0; i < job.sources.size(); ++i) {
      job.etag.clear(); job.ranged = false; job.known = false; job.total = 0;
      if (!probe_source(job, i)) return false;
      const auto& source = job.sources[i];
      for (std::uint64_t local = 0; local < source.piece.size;) {
        const auto length = source.ranged ? std::min(job.request.range_bytes, source.piece.size-local) : source.piece.size;
        if (job.segments.size() == max_ranges) { job.fail("too many manifest ranges; increase rangeBytes"); return false; }
        job.segments.push_back({source.piece.offset+local, length, i});
        local += length;
      }
    }
    job.total = job.request.expected_bytes; job.known = true; job.ranged = true;
  }
  if (job.ranged) {
    const auto count = job.sources.empty() ? (job.total+job.request.range_bytes-1)/job.request.range_bytes : job.segments.size();
    if (count > max_ranges) { job.fail("too many ranges; increase rangeBytes (maximum 65536 ranges)"); return false; }
    job.completed.resize(static_cast<std::size_t>(count));
    Hash hash;
    for (const auto& value : {job.request.url, job.etag, job.request.sha256}) {
      hash.update(value.data(), value.size()); hash.update("\0", 1);
    }
    for (const auto& source : job.sources) {
      for (const auto& value : {source.piece.url, source.etag, source.piece.sha1,
                               std::to_string(source.piece.offset), std::to_string(source.piece.size)}) {
        hash.update(value.data(), value.size()); hash.update("\0", 1);
      }
    }
    if (job.root_fd >= 0) {
      const auto identity = std::to_string(job.root_stat.st_dev)+":"+std::to_string(job.root_stat.st_ino);
      hash.update(identity.data(), identity.size());
    }
    job.identity = hash.finish();
    if (job.identity.empty()) { job.fail("checkpoint identity hash failed"); return false; }
  }
  struct stat existing;
  if (lstat(job.request.destination.c_str(), &existing) == 0 || errno != ENOENT) {
    job.fail("destination already exists or cannot be inspected; choose a new destination"); return false;
  }
  const std::string partial = job.request.destination+".part";
  job.fd = open(partial.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (job.fd < 0 && errno == EEXIST && job.request.resume && job.ranged) {
    job.fd = open(partial.c_str(), O_RDWR | O_NOFOLLOW);
    const int meta = open((job.request.destination+".resume").c_str(), O_RDONLY | O_NOFOLLOW);
    Checkpoint saved, expected;
    bool valid = job.fd >= 0 && meta >= 0 && fstat(job.fd, &existing) == 0 && S_ISREG(existing.st_mode) &&
      existing.st_size >= 0 && static_cast<std::uint64_t>(existing.st_size) <= job.total && read_all(meta, &saved, sizeof saved) &&
      !std::memcmp(saved.magic, expected.magic, sizeof saved.magic) && saved.total == job.total &&
      saved.range_bytes == job.request.range_bytes && saved.count == job.completed.size() &&
      !std::memcmp(saved.identity, job.identity.c_str(), 65) && read_all(meta, job.completed.data(), job.completed.size());
    if (meta >= 0) close(meta);
    for (unsigned char value : job.completed) if (value > 1) valid = false;
    if (!valid) { job.fail("partial file has no matching durable checkpoint; remove .part/.resume or choose another destination"); return false; }
    for (std::size_t i = 0; i < job.completed.size(); ++i) if (job.completed[i]) {
      const auto begin = job.sources.empty() ? i*job.request.range_bytes : job.segments[i].begin;
      const auto end = job.sources.empty() ? std::min(job.total, (i+1)*job.request.range_bytes) : begin+job.segments[i].length;
      if (end > static_cast<std::uint64_t>(existing.st_size)) { job.fail("partial file is shorter than its checkpoint"); return false; }
      job.committed += end-begin;
    }
  }
  if (job.fd < 0) { job.fail(system_error("open partial file (parent must exist; existing partial files require a valid checkpoint)")); return false; }
  struct stat partial_stat;
  if (fstat(job.fd, &partial_stat) != 0 || !S_ISREG(partial_stat.st_mode) || partial_stat.st_nlink != 1 ||
      (job.root_fd >= 0 && partial_stat.st_dev != job.root_stat.st_dev)) {
    job.fail("partial file must be a regular, unshared file on the selected storage"); return false;
  }
  // Establish ownership and a recoverable empty checkpoint before accepting file bytes.
  job.checkpoint_ready = true;
  if (job.ranged && !checkpoint(job, job.completed)) return false;
  return true;
}

void Service::writer_loop() {
  for (;;) {
    Write command;
    {
      std::unique_lock lock(disk_mutex_);
      disk_wake_.wait(lock, [&] { return writer_stopping_ || !writes_.empty(); });
      if (writes_.empty() && writer_stopping_) break;
      command = std::move(writes_.front()); writes_.pop_front();
    }
    Job& job = *command.job;
    if (command.block) {
      if (!job.failed && storage_matches(job) && write_all(job.fd, command.block->data, command.size, command.offset))
        command.transfer->written += command.size;
      else if (!job.failed) job.fail(system_error("write partial file"));
      release(command.block);
      --command.transfer->pending;
    } else {
      if (job.fd >= 0 && job.checkpoint_ready) checkpoint(job, command.checkpoint);
      if (command.finish) {
        if (!job.cancelled && !job.failed && !job.recovering && ftruncate(job.fd, static_cast<off_t>(job.total)) != 0)
          job.fail(system_error("truncate completed file"));
        if (!job.cancelled && !job.failed && verify_file(job)) {
          if (!job.recovering && fsync(job.fd) != 0) job.fail(system_error("sync completed file"));
          job.storage_checked = {};
          if (!job.cancelled && !job.failed && storage_matches(job)) {
            if (job.recovering) {
              struct stat visible{}, opened{};
              if (lstat(job.request.destination.c_str(), &visible) != 0 || fstat(job.fd, &opened) != 0 ||
                  visible.st_dev != opened.st_dev || visible.st_ino != opened.st_ino)
                job.fail("completed destination changed during recovery");
            } else publish_file(job);
          }
        }
        if (job.fd >= 0 && close(job.fd) != 0) job.fail(system_error("close partial file"));
        job.fd = -1;
        {
          std::lock_guard lock(job.mutex);
          job.snapshot.state = terminal_state(job);
          job.snapshot.connections = 0;
        }
        job.finalized = true;
      }
    }
    --job.pending;
    if (multi_) curl_multi_wakeup(multi_);
  }
}

void Service::snapshot(Job& job, unsigned allowed, Clock::time_point now,
                       Clock::time_point& previous, std::uint64_t& previous_written) {
  std::uint64_t written = job.committed, received = job.committed;
  unsigned active = 0;
  for (const Transfer& t : transfers_) if (t.active || t.draining) {
    written += t.written.load(); received += t.accepted; ++active;
  }
  std::uint64_t buffered;
  { std::lock_guard lock(disk_mutex_); buffered = (block_count-free_.size())*block_size; }
  const double elapsed = std::chrono::duration<double>(now-previous).count();
  const double speed = elapsed > 0 && written >= previous_written ? (written-previous_written)/elapsed : 0;
  {
    std::lock_guard lock(job.mutex);
    job.snapshot.received = received; job.snapshot.written = written;
    job.snapshot.total = job.total; job.snapshot.total_known = job.known;
    job.snapshot.bytes_per_second = speed; job.snapshot.buffered = buffered;
    job.snapshot.connections = std::min(active, allowed); job.snapshot.retries = job.retries;
  }
  previous = now; previous_written = written;
}

void Service::finish_transfer(Transfer& t) {
  Job& job = *t.job;
  if (t.block) flush(t);
  t.active = false; t.draining = true;
  if (t.result == CURLE_OK && !job.request.destination.empty()) {
    if (!valid_file_response(t)) t.error = "server changed the resource or returned an invalid range/encoding/status";
    else if ((job.ranged || job.known) && t.accepted != t.length) t.error = "truncated response";
    if (!job.known && !job.ranged) { job.total = t.accepted; job.known = true; }
  }
}

void Service::run_job(const std::shared_ptr<Job>& job) {
  for (Transfer& t : transfers_) {
    t.job = job; t.active = false; t.draining = false; t.probe = false; t.source = 0;
    t.accepted = 0; t.written = 0; t.pending = 0;
  }
  { std::lock_guard lock(job->mutex); job->snapshot.state = "connecting"; }
  const bool file = !job->request.destination.empty();
  const bool prepared = !file || prepare(*job);
  for (Transfer& t : transfers_) t.probe = false;
  if (prepared && !job->cancelled && !job->recovering) {
    { std::lock_guard lock(job->mutex); job->snapshot.state = "downloading"; }
    const unsigned limit = job->ranged ? job->request.connections : 1;
    unsigned allowed = job->request.adaptive ? std::min(4u, limit) : limit;
    const std::size_t count = job->ranged ? job->completed.size() : 1;
    std::vector<unsigned char> scheduled(count);
    std::vector<unsigned> attempts(count);
    std::vector<Clock::time_point> retry_at(count);
    for (std::size_t i = 0; i < count && job->ranged; ++i) scheduled[i] = job->completed[i];
    Clock::time_point previous = Clock::now(), adjusted = previous, checkpointed = previous;
    std::uint64_t previous_written = job->committed;
    double previous_speed = 0;
    std::size_t finished = static_cast<std::size_t>(std::count(scheduled.begin(), scheduled.end(), 1));
    while (finished < count && !job->failed && !job->cancelled) {
      const auto now = Clock::now();
      unsigned in_flight = 0;
      for (Transfer& t : transfers_) if (t.active || t.draining) ++in_flight;
      for (Transfer& t : transfers_) {
        if (t.draining && !t.pending) {
          t.draining = false; --in_flight;
          if (t.result == CURLE_OK && t.error.empty()) {
            job->committed += t.written;
            if (job->ranged) job->completed[t.piece] = 1;
            ++finished;
          } else if (attempts[t.piece] < 4 && retryable(t)) {
            ++job->retries;
            scheduled[t.piece] = 0;
            retry_at[t.piece] = now+std::chrono::milliseconds(std::max(t.retry_after*1000u,
                (250u << attempts[t.piece])+(t.piece*37)%200));
          } else {
            job->fail(t.error.empty() ? curl_easy_strerror(t.result) : t.error); break;
          }
        }
        if (t.active && t.paused) {
          bool available;
          { std::lock_guard lock(disk_mutex_); available = !free_.empty(); }
          if (available) {
            t.paused = false;
            const auto result = curl_easy_pause(t.curl, CURLPAUSE_CONT);
            if (result != CURLE_OK) job->fail(curl_easy_strerror(result));
          }
        }
        if (!t.active && !t.draining && in_flight < allowed) {
          std::size_t next = 0;
          while (next < count && (scheduled[next] || now < retry_at[next])) ++next;
          if (next == count) continue;
          t.piece = static_cast<unsigned>(next); scheduled[next] = 1;
          if (!job->sources.empty()) {
            const auto& segment = job->segments[next];
            t.begin = segment.begin; t.length = segment.length; t.source = segment.source;
          } else {
            t.begin = job->ranged ? t.piece*job->request.range_bytes : 0;
            t.length = job->ranged ? std::min(job->request.range_bytes, job->total-t.begin) : job->total;
          }
          t.accepted = 0; t.written = 0; ++attempts[next];
          if (!file) job->response.clear();
          if (!configure(t) || curl_multi_add_handle(multi_, t.curl) != CURLM_OK) {
            job->fail(t.error.empty() ? "could not schedule transfer" : t.error); break;
          }
          t.active = true; ++in_flight;
        }
      }
      int running = 0;
      const auto result = curl_multi_perform(multi_, &running);
      if (result != CURLM_OK) { job->fail(curl_multi_strerror(result)); break; }
      int left = 0;
      while (CURLMsg* message = curl_multi_info_read(multi_, &left)) if (message->msg == CURLMSG_DONE) {
        Transfer* t = nullptr;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &t);
        curl_multi_remove_handle(multi_, message->easy_handle);
        if (!t) { job->fail("missing transfer identity"); break; }
        t->result = message->data.result;
        { std::lock_guard lock(job->mutex); job->snapshot.status = static_cast<int>(t->status); }
        finish_transfer(*t);
      }
      if (now-previous >= std::chrono::milliseconds(250)) {
        snapshot(*job, allowed, now, previous, previous_written);
        if (job->request.adaptive && now-adjusted >= std::chrono::seconds(3)) {
          double speed; std::uint64_t buffered;
          { std::lock_guard lock(job->mutex); speed = job->snapshot.bytes_per_second; buffered = job->snapshot.buffered; }
          if (buffered > block_size*block_count*3/4 || (previous_speed > 0 && speed < previous_speed*0.85))
            allowed = std::max(1u, allowed-1);
          else if (!previous_speed || speed > previous_speed*1.05) allowed = std::min(limit, allowed+1);
          previous_speed = speed; adjusted = now;
        }
      }
      if (job->ranged && now-checkpointed >= std::chrono::seconds(5)) {
        queue_checkpoint(job, false); checkpointed = now;
      }
      if (finished < count) curl_multi_poll(multi_, nullptr, 0, 50, nullptr);
    }
    for (Transfer& t : transfers_) {
      if (t.active) { curl_multi_remove_handle(multi_, t.curl); t.active = false; }
      flush(t);
    }
    // Draining pointers must remain stable until the writer has returned their blocks.
    while (job->pending) curl_multi_poll(multi_, nullptr, 0, 50, nullptr);
    snapshot(*job, 0, Clock::now(), previous, previous_written);
    { std::lock_guard lock(job->mutex); job->snapshot.connections = 0; }
  }
  if (prepared && job->recovering) {
    std::lock_guard lock(job->mutex);
    job->snapshot.written = job->snapshot.received = job->total;
    job->snapshot.total = job->total; job->snapshot.total_known = true;
  }
  if (file && job->fd >= 0) {
    queue_checkpoint(job, true);
    while (!job->finalized) curl_multi_poll(multi_, nullptr, 0, 50, nullptr);
  } else {
    std::lock_guard lock(job->mutex);
    job->snapshot.body = std::move(job->response);
    if (!file) {
      job->snapshot.received = job->snapshot.body.size();
      job->snapshot.total = job->snapshot.body.size(); job->snapshot.total_known = true;
    }
    job->snapshot.state = terminal_state(*job);
    job->snapshot.connections = 0; job->finalized = true;
  }
}

void Service::network_loop() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || !waiting_.empty(); });
      if (stopping_ && waiting_.empty()) break;
      job = waiting_.front(); waiting_.pop_front();
    }
    run_job(job);
  }
}
} // namespace
bool start() { return service.start(); }
void stop() { service.stop(); }
std::uint32_t enqueue(Request request, std::string& error) { return service.enqueue(std::move(request), error); }
void cancel(std::uint32_t id) { service.cancel(id); }
std::vector<Snapshot> poll() { return service.poll(); }
const char* version() { return curl_version(); }
} // namespace network
#else
namespace network {
bool start() { return false; }
void stop() {}
std::uint32_t enqueue(Request, std::string& error) {
  error = "enable networking: true and filesystemAccess: console in app.json"; return 0;
}
void cancel(std::uint32_t) {}
std::vector<Snapshot> poll() { return {}; }
const char* version() { return "disabled"; }
} // namespace network
#endif
