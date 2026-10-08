// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "crash_log.hpp"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C" int __real_sceKernelDebugOutText(int channel, const char* text);

namespace {
std::atomic<int> log_fd{-1};
// A session that logs in a loop stops here rather than filling the drive.
constexpr long kMaxBytes = 16L * 1024 * 1024;
std::atomic<long> written{0};

void append(const char* text, std::size_t size) {
  const int fd = log_fd.load(std::memory_order_relaxed);
  if (fd < 0 || written.fetch_add(static_cast<long>(size)) > kMaxBytes) return;
  (void)!::write(fd, text, size);
}

// Async-signal-safe: no stdio, no allocation.
char* hex(char* out, std::uint64_t value) {
  *out++ = '0'; *out++ = 'x';
  for (int shift = 60; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 0xf];
  return out;
}

void fatal(int signal, siginfo_t* info, void* context) {
  char line[160];
  char* at = line;
  const char prefix[] = "[PS5-REACT] fatal signal ";
  std::memcpy(at, prefix, sizeof prefix - 1); at += sizeof prefix - 1;
  if (signal >= 10) *at++ = static_cast<char>('0' + signal / 10);
  *at++ = static_cast<char>('0' + signal % 10);
  const char address[] = " address "; std::memcpy(at, address, sizeof address - 1); at += sizeof address - 1;
  at = hex(at, reinterpret_cast<std::uintptr_t>(info ? info->si_addr : nullptr));
  const auto* uc = static_cast<const ucontext_t*>(context);
  const char rip[] = " rip "; std::memcpy(at, rip, sizeof rip - 1); at += sizeof rip - 1;
  at = hex(at, uc ? static_cast<std::uint64_t>(uc->uc_mcontext.mc_rip) : 0);
  const char rsp[] = " rsp "; std::memcpy(at, rsp, sizeof rsp - 1); at += sizeof rsp - 1;
  at = hex(at, uc ? static_cast<std::uint64_t>(uc->uc_mcontext.mc_rsp) : 0);
  *at++ = '\n';
  append(line, static_cast<std::size_t>(at - line));
  __real_sceKernelDebugOutText(0, line);
  ::signal(signal, SIG_DFL);
  ::raise(signal);
}
}

// Linked with --wrap=sceKernelDebugOutText: every klog line of the title, from any library.
extern "C" int __wrap_sceKernelDebugOutText(int channel, const char* text) {
  if (text) append(text, std::strlen(text));
  return __real_sceKernelDebugOutText(channel, text);
}

namespace crash_log {
void start(const char* directory) {
  char current[320], previous[320];
  std::snprintf(current, sizeof current, "%s/app.log", directory);
  std::snprintf(previous, sizeof previous, "%s/app.prev.log", directory);
  ::rename(current, previous);
  log_fd = ::open(current, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  struct sigaction action {};
  action.sa_sigaction = fatal;
  action.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(signal, &action, nullptr);
}
}
