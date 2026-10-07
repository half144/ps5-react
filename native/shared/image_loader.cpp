// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "image_loader.hpp"
#include "thread_name.hpp"
#include "network.hpp"
#ifdef PROSPERO
#include "app_config.hpp"
#endif
#if !defined(PROSPERO) || PS5_REACT_NETWORKING
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

// stb_image reports failures through one global string; only the decode thread decodes.
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#include "stb_image.h"
#pragma clang diagnostic pop

namespace images {
namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kConnections = 12, kAttempts = 3;

struct Entry {
  ~Entry() { std::free(pixels); }
  std::uint32_t id = 0;
  std::string key, url;
  int box_width = 0, box_height = 0;
  Fit fit = Fit::cover;
  std::atomic<bool> cancelled{false};
  // Drawn by an element, not only prefetched: fetched and decoded before prefetches.
  std::atomic<bool> wanted{false};
  // Written by the workers before the entry is handed to poll() under the service mutex.
  std::shared_ptr<const std::string> body;
  std::uint32_t* pixels = nullptr;
  int width = 0, height = 0;
  bool opaque = true;
  std::int32_t color = -1;
  std::string error;
  unsigned attempts = 0;
  Clock::time_point retry_at{};
  // Render thread only.
  unsigned refs = 0;
  std::uint64_t used = 0;
  bool reported = false;
};
using EntryPtr = std::shared_ptr<Entry>;

// One request per URL: loads of the same URL for other boxes join it and decode its bytes.
struct Transfer {
  CURL* curl = nullptr;
  std::string url, body;
  std::vector<EntryPtr> entries;
  bool overflow = false;
};

// Recently fetched encoded bytes by URL, so another box for the same image decodes without a fetch.
struct Encoded {
  std::string url;
  std::shared_ptr<const std::string> body;
  std::uint64_t used = 0;
};

std::string message(const Entry& entry, const std::string& reason) {
  return "Image.load " + entry.url + ": " + reason;
}

// Box filter over a source span: output i averages source [i*scale, (i+1)*scale), partial
// pixels weighted by coverage. The scale is at least 1; images are never enlarged here.
struct Taps {
  std::vector<int> first, count;
  std::vector<float> weights;
  Taps(int offset, int source, int target) {
    const double scale = static_cast<double>(source) / target;
    for (int i = 0; i < target; ++i) {
      const double a = i * scale, b = std::min<double>(source, (i + 1) * scale);
      const int begin = static_cast<int>(a), end = std::min(source, static_cast<int>(std::ceil(b)));
      first.push_back(offset + begin);
      count.push_back(std::max(end - begin, 1));
      for (int j = begin; j < begin + count.back(); ++j)
        weights.push_back(static_cast<float>((std::min<double>(b, j + 1) - std::max<double>(a, j)) / scale));
    }
  }
};

struct Plan { int x = 0, y = 0, width = 0, height = 0, out_width = 0, out_height = 0; };

// The source crop and output size for a box: cover crops to the box's aspect ratio, contain keeps
// the whole image; both shrink to the box but never enlarge, leaving upscaling to the engine.
Plan plan(int width, int height, int box_width, int box_height, Fit fit) {
  Plan p{0, 0, width, height, width, height};
  if (fit == Fit::none) return p;
  if (fit == Fit::cover) {
    if (static_cast<std::int64_t>(width) * box_height > static_cast<std::int64_t>(height) * box_width)
      p.width = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * box_width / box_height)));
    else
      p.height = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * box_height / box_width)));
    p.x = (width - p.width) / 2; p.y = (height - p.height) / 2;
    p.out_width = p.width; p.out_height = p.height;
    if (p.width > box_width) { p.out_width = box_width; p.out_height = box_height; }
  } else if (fit == Fit::contain) {
    const double scale = std::min({1.0, static_cast<double>(box_width) / width, static_cast<double>(box_height) / height});
    p.out_width = std::max(1, static_cast<int>(std::lround(width * scale)));
    p.out_height = std::max(1, static_cast<int>(std::lround(height * scale)));
  } else {
    p.out_width = std::min(width, box_width); p.out_height = std::min(height, box_height);
  }
  return p;
}

// Resamples the planned crop of 8-bit RGB or RGBA rows into premultiplied ARGB8888.
bool resample(const unsigned char* source, int width, int channels, const Plan& p, Entry& entry) {
  const std::size_t count = static_cast<std::size_t>(p.out_width) * p.out_height;
  auto* out = static_cast<std::uint32_t*>(std::malloc(count * sizeof(std::uint32_t)));
  if (!out) return false;
  const Taps columns(p.x, p.width, p.out_width), rows(p.y, p.height, p.out_height);
  std::vector<std::size_t> column_weight(p.out_width);
  for (int x = 0, w = 0; x < p.out_width; w += columns.count[x], ++x) column_weight[x] = static_cast<std::size_t>(w);
  std::vector<float> line(static_cast<std::size_t>(p.out_width) * 4), sum(line.size());
  bool opaque = true;
  for (int y = 0, row_weight = 0; y < p.out_height; row_weight += rows.count[y], ++y) {
    std::fill(sum.begin(), sum.end(), 0.0f);
    for (int k = 0; k < rows.count[y]; ++k) {
      const float wy = rows.weights[static_cast<std::size_t>(row_weight + k)];
      const unsigned char* row = source + static_cast<std::size_t>(rows.first[y] + k) * width * channels;
      for (int x = 0; x < p.out_width; ++x) {
        float r = 0, g = 0, b = 0, a = 0;
        const float* weight = &columns.weights[column_weight[x]];
        const unsigned char* px = row + static_cast<std::size_t>(columns.first[x]) * channels;
        for (int i = 0; i < columns.count[x]; ++i, px += channels) {
          const float alpha = channels == 4 ? px[3] : 255.0f, w = weight[i] * alpha / 255.0f;
          r += w * px[0]; g += w * px[1]; b += w * px[2]; a += weight[i] * alpha;
        }
        float* s = &sum[static_cast<std::size_t>(x) * 4];
        s[0] += wy * a; s[1] += wy * r; s[2] += wy * g; s[3] += wy * b;
      }
    }
    std::uint32_t* target = out + static_cast<std::size_t>(y) * p.out_width;
    for (int x = 0; x < p.out_width; ++x) {
      const float* s = &sum[static_cast<std::size_t>(x) * 4];
      const auto a = static_cast<std::uint32_t>(std::clamp(std::lround(s[0]), 0L, 255L));
      const auto channel = [&](float value) { return std::min(static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 255L)), a); };
      opaque = opaque && a == 255;
      target[x] = a << 24 | channel(s[1]) << 16 | channel(s[2]) << 8 | channel(s[3]);
    }
  }
  entry.pixels = out; entry.width = p.out_width; entry.height = p.out_height; entry.opaque = opaque;
  return true;
}

// The most prominent saturated hue, raised to full brightness and to at least 85% saturation: what a
// light bar or accent can show of the art (an LED shows a pale tint as a washed-out colour). -1 when
// the art has no vivid colour. Samples at most about 16k pixels.
std::int32_t vivid_color(const std::uint32_t* pixels, int width, int height) {
  constexpr int bins = 24;
  float weight[bins] = {}, red[bins] = {}, green[bins] = {}, blue[bins] = {};
  const int count = width * height, step = std::max(1, count / 16384);
  int sampled = 0;
  for (int i = 0; i < count; i += step) {
    ++sampled;
    const std::uint32_t pixel = pixels[i];
    const float alpha = static_cast<float>(pixel >> 24);
    if (alpha < 128) continue;
    const float r = ((pixel >> 16) & 255) / alpha, g = ((pixel >> 8) & 255) / alpha, b = (pixel & 255) / alpha;
    const float high = std::max({r, g, b}), low = std::min({r, g, b}), range = high - low;
    if (high < 0.25f || range < 0.25f * high) continue;
    float hue = high == r ? (g - b) / range : high == g ? 2 + (b - r) / range : 4 + (r - g) / range;
    if (hue < 0) hue += 6;
    const int bin = std::min(bins - 1, static_cast<int>(hue / 6 * bins));
    weight[bin] += range;
    // Weighted by saturation, so the purest pixels of a hue set its colour rather than its pale edges.
    red[bin] += r * range; green[bin] += g * range; blue[bin] += b * range;
  }
  const int best = static_cast<int>(std::max_element(weight, weight + bins) - weight);
  if (weight[best] < 0.01f * sampled) return -1;
  const float peak = std::max({red[best], green[best], blue[best]});
  const float floor = std::min({red[best], green[best], blue[best]}), saturation = (peak - floor) / peak;
  // Scaling each channel's distance below the peak keeps the hue while raising the saturation.
  const float boost = std::max(1.0f, 0.85f / saturation);
  const auto channel = [&](float value) {
    return static_cast<std::int32_t>(std::lround(std::max(0.0f, peak - (peak - value) * boost) / peak * 255));
  };
  return channel(red[best]) << 16 | channel(green[best]) << 8 | channel(blue[best]);
}

void decode(Entry& entry) {
  const auto* bytes = reinterpret_cast<const stbi_uc*>(entry.body->data());
  const int size = static_cast<int>(entry.body->size());
  int width = 0, height = 0, components = 0;
  if (!stbi_info_from_memory(bytes, size, &width, &height, &components)) {
    entry.error = message(entry, std::string("not a decodable JPEG or PNG (") + stbi_failure_reason() + ")");
  } else if (static_cast<std::uint64_t>(width) * height > kMaxSourcePixels) {
    entry.error = message(entry, std::to_string(width) + "x" + std::to_string(height) +
                          " exceeds the 5-megapixel source limit");
  }
  if (!entry.error.empty()) { entry.body.reset(); return; }
  const int channels = components == 2 || components == 4 ? 4 : 3;
  stbi_uc* source = stbi_load_from_memory(bytes, size, &width, &height, &components, channels);
  entry.body.reset();
  if (!source) { entry.error = message(entry, std::string("decode failed (") + stbi_failure_reason() + ")"); return; }
  const Plan p = plan(width, height, entry.box_width, entry.box_height, entry.fit);
  if (!resample(source, width, channels, p, entry)) entry.error = message(entry, "out of memory for decoded pixels");
  else entry.color = vivid_color(entry.pixels, entry.width, entry.height);
  stbi_image_free(source);
}

// Encoded responses kept on disk between launches, keyed by a hash of the URL. Each file holds a
// header (magic, fetch time, URL) and the body; the fetch thread alone touches the directory.
class DiskCache {
public:
  void open(const std::string& directory) {
    directory_ = directory;
    if (directory_.empty() || !make_directories(directory_)) { directory_.clear(); return; }
    files_.clear(); total_ = 0;
    if (DIR* dir = opendir(directory_.c_str())) {
      while (dirent* item = readdir(dir)) {
        struct stat info;
        if (item->d_name[0] == '.' || stat(path(item->d_name).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        files_.push_back({item->d_name, static_cast<std::size_t>(info.st_size), static_cast<std::int64_t>(info.st_mtime)});
        total_ += static_cast<std::size_t>(info.st_size);
      }
      closedir(dir);
    }
    trim();
  }

  std::shared_ptr<const std::string> read(const std::string& url) {
    if (directory_.empty()) return nullptr;
    const std::string name = key(url);
    File* file = find(name);
    if (!file) return nullptr;
    const int fd = ::open(path(name).c_str(), O_RDONLY);
    std::string bytes(file->size, '\0');
    const bool whole = fd >= 0 && ::read(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
    if (fd >= 0) close(fd);
    Header header{};
    if (!whole || bytes.size() < sizeof header) return nullptr;
    std::memcpy(&header, bytes.data(), sizeof header);
    const std::size_t start = sizeof header + header.url_size;
    if (std::memcmp(header.magic, kMagic, sizeof header.magic) || start > bytes.size() ||
        bytes.compare(sizeof header, header.url_size, url) || std::time(nullptr) - header.fetched > kMaxAgeSeconds) {
      remove(name);
      return nullptr;
    }
    file->used = std::time(nullptr);
    utimes(path(name).c_str(), nullptr);
    return std::make_shared<const std::string>(bytes.substr(start));
  }

  void write(const std::string& url, const std::string& body) {
    if (directory_.empty() || body.size() > kDiskCacheBytes / 4) return;
    const std::string name = key(url), temporary = path(name + ".tmp");
    Header header{};
    std::memcpy(header.magic, kMagic, sizeof header.magic);
    header.fetched = std::time(nullptr);
    header.url_size = static_cast<std::uint32_t>(url.size());
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    const bool ok = ::write(fd, &header, sizeof header) == static_cast<ssize_t>(sizeof header) &&
      ::write(fd, url.data(), url.size()) == static_cast<ssize_t>(url.size()) &&
      ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
    if (close(fd) != 0 || !ok || rename(temporary.c_str(), path(name).c_str()) != 0) { unlink(temporary.c_str()); return; }
    const std::size_t size = sizeof header + url.size() + body.size();
    if (File* old = find(name)) { total_ -= old->size; old->size = size; old->used = header.fetched; }
    else files_.push_back({name, size, header.fetched});
    total_ += size;
    trim();
  }

private:
  static constexpr char kMagic[8] = {'P', '5', 'R', 'I', 'M', 'G', '0', '1'};
  static constexpr std::int64_t kMaxAgeSeconds = 7 * 24 * 3600;
  struct Header { char magic[8]; std::int64_t fetched; std::uint32_t url_size, reserved; };
  struct File { std::string name; std::size_t size; std::int64_t used; };

  static std::string key(const std::string& url) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : url) hash = (hash ^ c) * 1099511628211ULL;
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
  }
  static bool make_directories(const std::string& directory) {
    for (std::size_t at = 1; at != std::string::npos;) {
      at = directory.find('/', at + 1);
      const std::string part = directory.substr(0, at);
      if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
  }
  std::string path(const std::string& name) const { return directory_ + "/" + name; }
  File* find(const std::string& name) {
    for (File& file : files_) if (file.name == name) return &file;
    return nullptr;
  }
  void remove(const std::string& name) {
    unlink(path(name).c_str());
    for (auto it = files_.begin(); it != files_.end(); ++it)
      if (it->name == name) { total_ -= it->size; files_.erase(it); return; }
  }
  // Least recently used files go first once the directory is over its cap.
  void trim() {
    while (total_ > kDiskCacheBytes && !files_.empty()) {
      auto oldest = std::min_element(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.used < b.used; });
      remove(std::string(oldest->name));
    }
  }

  std::string directory_;
  std::vector<File> files_;
  std::size_t total_ = 0;
};

bool spawn(pthread_t& thread, void* (*body)(void*), void* data) {
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  const int stack = pthread_attr_setstacksize(&attributes, 1024 * 1024);
  const int result = stack ? stack : pthread_create(&thread, &attributes, body, data);
  pthread_attr_destroy(&attributes);
  return result == 0;
}

class Service {
public:
  bool start(const std::string& cache_directory) {
    if (running_) return true;
    cache_directory_ = cache_directory;
    stopping_ = false;
    multi_ = curl_multi_init();
    if (!multi_) return false;
    for (Transfer& t : transfers_) if (!(t.curl = curl_easy_init())) { stop(nullptr); return false; }
    fetch_started_ = spawn(fetch_thread_, +[](void* p)->void* { static_cast<Service*>(p)->fetch_loop(); return nullptr; }, this);
    decode_started_ = fetch_started_ &&
      spawn(decode_thread_, +[](void* p)->void* { static_cast<Service*>(p)->decode_loop(); return nullptr; }, this);
    if (!decode_started_) { stop(nullptr); return false; }
    running_ = true; return true;
  }

  void stop(void (*evict)(std::uint32_t)) {
    for (const auto& [id, entry] : by_id_) if (entry->reported && entry->pixels) evict(id);
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (multi_) curl_multi_wakeup(multi_);
    if (fetch_started_) pthread_join(fetch_thread_, nullptr);
    if (decode_started_) pthread_join(decode_thread_, nullptr);
    fetch_started_ = decode_started_ = false;
    for (Transfer& t : transfers_) {
      if (t.curl) curl_easy_cleanup(t.curl);
      t = Transfer{};
    }
    if (multi_) curl_multi_cleanup(multi_);
    multi_ = nullptr;
    fetches_.clear(); decodes_.clear(); finished_.clear(); by_key_.clear(); by_id_.clear(); encoded_.clear();
    running_ = false;
  }

  std::uint32_t load(const std::string& url, int width, int height, Fit fit, bool prefetch, Result& result, std::string& error) {
    if (!running_) { error = "Image.load " + url + ": networking is not available"; return 0; }
    const std::string key = std::to_string(static_cast<int>(fit)) + ' ' + std::to_string(width) + 'x' +
                            std::to_string(height) + ' ' + url;
    auto found = by_key_.find(key);
    EntryPtr entry;
    if (found != by_key_.end()) {
      entry = found->second;
    } else {
      entry = std::make_shared<Entry>();
      entry->id = next_id_++; entry->key = key; entry->url = url;
      entry->box_width = width; entry->box_height = height; entry->fit = fit;
      by_key_.emplace(key, entry); by_id_.emplace(entry->id, entry);
      { std::lock_guard lock(mutex_); fetches_.push_back(entry); }
      curl_multi_wakeup(multi_);
    }
    if (!prefetch) entry->wanted = true;
    ++entry->refs; entry->used = ++clock_;
    result = report(*entry);
    return entry->id;
  }

  void discard(std::uint32_t id, const std::string& reason) {
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return;
    Entry& entry = *found->second;
    std::free(entry.pixels);
    entry.pixels = nullptr;
    entry.error = message(entry, reason);
  }

  void release(std::uint32_t id) {
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return;
    const EntryPtr entry = found->second;
    if (--entry->refs) return;
    entry->used = ++clock_;
    if (entry->reported && entry->pixels) { over_budget_ = true; return; }
    entry->cancelled = true;
    forget(*entry);
    if (!entry->reported) curl_multi_wakeup(multi_);
  }

  std::vector<Result> poll(void (*evict)(std::uint32_t)) {
    std::vector<EntryPtr> done;
    { std::lock_guard lock(mutex_); done.swap(finished_); }
    std::vector<Result> out;
    for (const EntryPtr& entry : done) {
      if (entry->cancelled) continue;
      entry->reported = true;
      out.push_back(report(*entry));
      over_budget_ = over_budget_ || entry->pixels;
    }
    if (over_budget_) trim(evict);
    return out;
  }

private:
  static std::size_t bytes(const Entry& entry) { return static_cast<std::size_t>(entry.width) * entry.height * 4; }

  static Result report(const Entry& entry) {
    Result r;
    r.id = entry.id;
    if (!entry.reported) return r;
    r.failed = !entry.pixels; r.ready = !r.failed; r.error = entry.error;
    r.width = entry.width; r.height = entry.height; r.opaque = entry.opaque; r.pixels = entry.pixels;
    r.color = entry.color;
    return r;
  }

  void forget(const Entry& entry) {
    by_key_.erase(entry.key);
    by_id_.erase(entry.id);
  }

  // Evicts unused images, least recently used first, until the cache fits its budget.
  void trim(void (*evict)(std::uint32_t)) {
    over_budget_ = false;
    std::size_t total = 0, decoded = 0;
    for (const auto& [id, entry] : by_id_) if (entry->reported && entry->pixels) { total += bytes(*entry); ++decoded; }
    while (total > kCacheBytes || decoded > kCacheEntries) {
      EntryPtr oldest;
      for (const auto& [id, entry] : by_id_)
        if (entry->reported && entry->pixels && !entry->refs && (!oldest || entry->used < oldest->used)) oldest = entry;
      if (!oldest) return;
      evict(oldest->id);
      total -= bytes(*oldest); --decoded;
      forget(*oldest);
    }
  }

  void finish(const EntryPtr& entry) {
    { std::lock_guard lock(mutex_); finished_.push_back(entry); }
  }

  static std::size_t body(char* data, std::size_t size, std::size_t count, void* opaque) {
    auto& t = *static_cast<Transfer*>(opaque);
    const std::size_t length = size * count;
    if (length > kMaxEncodedBytes - t.body.size()) { t.overflow = true; return 0; }
    t.body.append(data, length);
    return length;
  }

  // Aborts a transfer once every load waiting for it has been released.
  static int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& entries = static_cast<Transfer*>(opaque)->entries;
    return std::all_of(entries.begin(), entries.end(), [](const EntryPtr& e) { return e->cancelled.load(); }) ? 1 : 0;
  }

  void fail(const EntryPtr& entry, const std::string& reason) {
    entry->error = message(*entry, reason);
    finish(entry);
  }

  void decode_later(std::vector<EntryPtr> entries, const std::shared_ptr<const std::string>& body) {
    {
      std::lock_guard lock(mutex_);
      for (EntryPtr& entry : entries) if (!entry->cancelled) { entry->body = body; decodes_.push_back(std::move(entry)); }
    }
    wake_.notify_all();
  }

  std::shared_ptr<const std::string> cached(const std::string& url) {
    for (Encoded& e : encoded_) if (e.url == url) { e.used = ++fetch_clock_; return e.body; }
    return nullptr;
  }

  void remember(const std::string& url, std::shared_ptr<const std::string> body) {
    std::size_t total = body->size();
    for (const Encoded& e : encoded_) total += e.body->size();
    while (total > kEncodedCacheBytes && !encoded_.empty()) {
      auto oldest = std::min_element(encoded_.begin(), encoded_.end(),
                                     [](const Encoded& a, const Encoded& b) { return a.used < b.used; });
      total -= oldest->body->size();
      encoded_.erase(oldest);
    }
    if (total <= kEncodedCacheBytes) encoded_.push_back({url, std::move(body), ++fetch_clock_});
  }

  // Fetch thread: serves the entry from cached bytes, joins a transfer of its URL, or starts one.
  void start_fetch(EntryPtr entry, unsigned& active) {
    if (auto body = cached(entry->url)) { decode_later({std::move(entry)}, body); return; }
    if (auto body = disk_.read(entry->url)) {
      remember(entry->url, body);
      decode_later({std::move(entry)}, body);
      return;
    }
    for (Transfer& t : transfers_) if (!t.entries.empty() && t.url == entry->url) { t.entries.push_back(std::move(entry)); return; }
    Transfer* t = nullptr;
    for (Transfer& candidate : transfers_) if (candidate.entries.empty()) { t = &candidate; break; }
    t->url = entry->url; t->body.clear(); t->overflow = false;
    t->entries.push_back(std::move(entry));
    curl_easy_reset(t->curl);
    bool ok = network::configure_transport(t->curl, t->url, true);
    const auto set = [&](CURLoption option, auto value) { ok = ok && curl_easy_setopt(t->curl, option, value) == CURLE_OK; };
    set(CURLOPT_TIMEOUT, 30L);
    set(CURLOPT_WRITEFUNCTION, body); set(CURLOPT_WRITEDATA, t);
    set(CURLOPT_XFERINFOFUNCTION, progress); set(CURLOPT_XFERINFODATA, t);
    set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_PRIVATE, t);
    if (ok && curl_multi_add_handle(multi_, t->curl) == CURLM_OK) { ++active; return; }
    for (const EntryPtr& e : t->entries) fail(e, "could not configure the HTTP transport");
    t->entries.clear();
  }

  void complete(Transfer& t, CURLcode result) {
    curl_multi_remove_handle(multi_, t.curl);
    std::vector<EntryPtr> entries;
    entries.swap(t.entries);
    std::erase_if(entries, [](const EntryPtr& e) { return e->cancelled.load(); });
    long status = 0;
    curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &status);
    const bool transient = result == CURLE_COULDNT_CONNECT || result == CURLE_OPERATION_TIMEDOUT ||
      result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR || result == CURLE_PARTIAL_FILE ||
      status == 429 || status == 502 || status == 503 || status == 504;
    std::string reason;
    if (t.overflow) reason = "response exceeds 8 MiB";
    else if (result != CURLE_OK) reason = curl_easy_strerror(result);
    else if (status != 200) reason = "HTTP " + std::to_string(status);
    if (reason.empty()) {
      auto body = std::make_shared<const std::string>(std::move(t.body));
      remember(t.url, body);
      disk_.write(t.url, *body);
      decode_later(std::move(entries), body);
    } else {
      std::lock_guard lock(mutex_);
      for (EntryPtr& entry : entries) {
        if (transient && ++entry->attempts < kAttempts) {
          entry->retry_at = Clock::now() + std::chrono::milliseconds(500 * entry->attempts);
          fetches_.push_back(std::move(entry));
        } else {
          entry->error = message(*entry, reason);
          finished_.push_back(std::move(entry));
        }
      }
    }
    std::string().swap(t.body);
  }

  void fetch_loop() {
    name_thread("img-fetch");
    disk_.open(cache_directory_);
    unsigned active = 0;
    while (true) {
      std::vector<EntryPtr> starting;
      {
        std::lock_guard lock(mutex_);
        if (stopping_) break;
        const auto now = Clock::now();
        // Loads joining a transfer or served from cache take no connection; this bound is loose.
        // Images on screen first, then prefetches, each in request order.
        for (const bool prefetches : {false, true})
          for (std::size_t n = fetches_.size(); n && active + starting.size() < kConnections; --n) {
            EntryPtr entry = std::move(fetches_.front());
            fetches_.pop_front();
            if (entry->cancelled) continue;
            if (entry->retry_at > now || (!prefetches && !entry->wanted)) fetches_.push_back(std::move(entry));
            else starting.push_back(std::move(entry));
          }
      }
      for (EntryPtr& entry : starting) start_fetch(std::move(entry), active);
      int running = 0;
      curl_multi_perform(multi_, &running);
      int left = 0;
      while (CURLMsg* done = curl_multi_info_read(multi_, &left)) {
        if (done->msg != CURLMSG_DONE) continue;
        Transfer* t = nullptr;
        curl_easy_getinfo(done->easy_handle, CURLINFO_PRIVATE, &t);
        complete(*t, done->data.result);
        --active;
      }
      curl_multi_poll(multi_, nullptr, 0, 100, nullptr);
    }
    for (Transfer& t : transfers_) if (!t.entries.empty()) { curl_multi_remove_handle(multi_, t.curl); t.entries.clear(); }
  }

  void decode_loop() {
    name_thread("img-decode");
    while (true) {
      EntryPtr entry;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || !decodes_.empty(); });
        if (stopping_) break;
        auto next = std::find_if(decodes_.begin(), decodes_.end(), [](const EntryPtr& e) { return e->wanted.load(); });
        if (next == decodes_.end()) next = decodes_.begin();
        entry = std::move(*next);
        decodes_.erase(next);
      }
      if (entry->cancelled) continue;
      decode(*entry);
      finish(entry);
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<EntryPtr> fetches_, decodes_;
  std::vector<EntryPtr> finished_;
  std::array<Transfer, kConnections> transfers_{};
  // Fetch thread only.
  DiskCache disk_;
  std::vector<Encoded> encoded_;
  std::uint64_t fetch_clock_ = 0;
  CURLM* multi_ = nullptr;
  pthread_t fetch_thread_{}, decode_thread_{};
  std::string cache_directory_;
  bool stopping_ = false, running_ = false, fetch_started_ = false, decode_started_ = false, over_budget_ = false;
  // Render thread only.
  std::unordered_map<std::string, EntryPtr> by_key_;
  std::unordered_map<std::uint32_t, EntryPtr> by_id_;
  std::uint32_t next_id_ = 1;
  std::uint64_t clock_ = 0;
};
Service service;
} // namespace

bool start(const std::string& cache_directory) { return service.start(cache_directory); }
void stop(void (*evict)(std::uint32_t)) { service.stop(evict); }
void discard(std::uint32_t id, const std::string& reason) { service.discard(id, reason); }
std::uint32_t load(const std::string& url, int width, int height, Fit fit, bool prefetch, Result& result,
                   std::string& error) {
  return service.load(url, width, height, fit, prefetch, result, error);
}
void release(std::uint32_t id) { service.release(id); }
std::vector<Result> poll(void (*evict)(std::uint32_t)) { return service.poll(evict); }
} // namespace images
#else
namespace images {
bool start(const std::string&) { return false; }
void stop(void (*)(std::uint32_t)) {}
void discard(std::uint32_t, const std::string&) {}
std::uint32_t load(const std::string& url, int, int, Fit, bool, Result&, std::string& error) {
  error = "Image.load " + url + ": enable networking: true and filesystemAccess: console in app.json"; return 0;
}
void release(std::uint32_t) {}
std::vector<Result> poll(void (*)(std::uint32_t)) { return {}; }
} // namespace images
#endif
