// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "packages.hpp"
#include "host_api.hpp"
#include "worker_thread.hpp"
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

// An app may not call the console's app-install service, and a loader payload may. Each install sends
// pkg-installer.elf (native/ps5/payloads/pkg_installer.c) to the payload loader on 127.0.0.1:9021, which
// gives it the rest of the connection as its standard input and output, and relays its lines.
extern "C" {
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
int sceNetConnect(int socket, const void* address, std::uint32_t length);
int sceNetSend(int socket, const void* data, std::size_t length, int flags);
int sceNetRecv(int socket, void* data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value, std::uint32_t size);
}

namespace packages {
namespace {
struct NetAddress { std::uint8_t length, family; std::uint16_t port; std::uint32_t address; std::uint16_t virtual_port; std::uint8_t zero[6]; };

struct Job {
  std::string path, name;
  Snapshot snapshot;
  std::atomic<bool> cancelled{false};
  std::atomic<int> socket{-1};
  std::mutex mutex;
  WorkerThread worker;
};
std::mutex guard;
std::shared_ptr<Job> active;
std::uint32_t next_id = 1;

bool terminal(const std::string& state) { return state == "completed" || state == "failed" || state == "cancelled"; }

void finish(Job& job, const char* state, const std::string& error = {}) {
  std::lock_guard lock(job.mutex);
  job.snapshot.state = job.cancelled ? "cancelled" : state;
  job.snapshot.error = error;
}

bool send_all(int socket, const char* data, std::size_t size) {
  while (size) {
    const int count = sceNetSend(socket, data, size, 0);
    if (count <= 0) return false;
    data += count; size -= static_cast<std::size_t>(count);
  }
  return true;
}

// Sends the program and the request; -1 with `error` set when the loader is not there.
int submit(Job& job, std::string& error) {
  char program[PATH_MAX];
  const int file = host::resolve_path("/app0/pkg-installer.elf", program, sizeof program) ? open(program, O_RDONLY) : -1;
  if (file < 0) { error = "Packages.install: pkg-installer.elf is missing from the app folder"; return -1; }
  const int socket = sceNetSocket("ps5-react-pkg", 2, 1, 6);
  // The payload reports once a second; half a minute of silence means it is gone.
  constexpr int timeout_us = 30'000'000, connect_us = 5'000'000, level = 0xffff;
  const NetAddress address{sizeof(NetAddress), 2, static_cast<std::uint16_t>((9021 << 8) | (9021 >> 8)), 0x0100007f, 0, {}};
  bool ok = socket >= 0 && sceNetSetsockopt(socket, level, 0x1105, &timeout_us, sizeof timeout_us) >= 0 &&
            sceNetSetsockopt(socket, level, 0x1106, &timeout_us, sizeof timeout_us) >= 0 &&
            sceNetSetsockopt(socket, level, 0x1109, &connect_us, sizeof connect_us) >= 0 &&
            sceNetConnect(socket, &address, sizeof address) >= 0;
  if (!ok) error = "Packages.install: the payload loader on port 9021 did not answer; load an ELF loader and retry";
  char buffer[65536];
  for (ssize_t count; ok && (count = read(file, buffer, sizeof buffer)) != 0;)
    ok = count > 0 && send_all(socket, buffer, static_cast<std::size_t>(count));
  close(file);
  const std::string request = "PKI1\n" + job.path + "\n" + job.name + "\n";
  if (ok && !send_all(socket, request.data(), request.size())) { ok = false; error = "Packages.install: the installer could not be sent"; }
  if (!ok) { if (socket >= 0) sceNetSocketClose(socket); return -1; }
  return socket;
}

// Lines: "ready", "started <content ID>", "p <written> <total> <status>", "ok", "fail <hex code> <reason>".
// Anything else is the install library's own output and is skipped.
bool handle(Job& job, const std::string& line) {
  std::lock_guard lock(job.mutex);
  Snapshot& s = job.snapshot;
  if (line.starts_with("started ")) { s.content_id = line.substr(8); s.state = "installing"; }
  else if (line.starts_with("p ")) {
    char status[32] = {};
    unsigned long long written = 0, total = 0;
    if (std::sscanf(line.c_str() + 2, "%llu %llu %31s", &written, &total, status) >= 2) { s.written = written; s.total = total; s.status = status; }
  } else if (line == "ok") { s.state = "completed"; s.written = s.total; s.status = "playable"; return true; }
  else if (line.starts_with("fail ")) {
    s.state = job.cancelled ? "cancelled" : "failed";
    s.error = "Packages.install " + job.path + ": " + (line.size() > 14 ? line.substr(14) : std::string("refused")) +
              " (0x" + line.substr(5, 8) + ")";
    return true;
  }
  return false;
}

void run(std::shared_ptr<Job> job) {
  std::string error;
  const int socket = submit(*job, error);
  if (socket < 0) { finish(*job, "failed", error); return; }
  job->socket = socket;
  std::string line;
  char buffer[1024];
  bool ended = false;
  while (!ended && !job->cancelled) {
    const int count = sceNetRecv(socket, buffer, sizeof buffer, 0);
    if (count <= 0) break;
    for (int i = 0; i < count && !ended; ++i) {
      if (buffer[i] != '\n') { if (line.size() < 2048) line.push_back(buffer[i]); continue; }
      ended = handle(*job, line);
      line.clear();
    }
  }
  if (job->socket.exchange(-1) >= 0) sceNetSocketClose(socket);
  if (!ended) finish(*job, "failed", "Packages.install " + job->path + ": the installer stopped answering; check the console's Downloads");
}
}

std::uint32_t install(std::string path, std::string name, std::string& error) {
  std::lock_guard lock(guard);
  if (active) { error = "A package is already installing; poll its completion first."; return 0; }
  auto job = std::make_shared<Job>();
  job->path = std::move(path);
  job->name = std::move(name);
  job->snapshot.id = next_id++;
  active = job;
  if (!job->worker.start([job] { run(job); }, 1024 * 1024)) {
    active.reset();
    error = "Cannot start the installer thread.";
    return 0;
  }
  return job->snapshot.id;
}

void cancel(std::uint32_t id) {
  std::lock_guard lock(guard);
  if (!active || active->snapshot.id != id) return;
  active->cancelled = true;
  // Unblocks the receive; the console keeps installing what it accepted.
  if (const int socket = active->socket.exchange(-1); socket >= 0) sceNetSocketClose(socket);
}

std::vector<Snapshot> poll() {
  std::lock_guard lock(guard);
  if (!active) return {};
  Snapshot snapshot;
  { std::lock_guard job_lock(active->mutex); snapshot = active->snapshot; }
  if (terminal(snapshot.state)) { active->worker.join(); active.reset(); }
  return {snapshot};
}

void stop() {
  std::shared_ptr<Job> job;
  { std::lock_guard lock(guard); job = active; }
  if (!job) return;
  cancel(job->snapshot.id);
  job->worker.join();
  std::lock_guard lock(guard);
  active.reset();
}
}
