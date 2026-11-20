// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SystemdNotify.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace luxir::systemd {

namespace {

class Fd {
public:
  int fd;
  explicit Fd(int fd) : fd(fd) {}
  ~Fd() { if (fd >= 0) ::close(fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

bool parseUnsigned(const char* s, unsigned long long& out) {
  if (s == nullptr || *s == 0) return false;
  auto end = s + std::strlen(s);
  auto [ptr, ec] = std::from_chars(s, end, out);
  return ec == std::errc() && ptr == end;
}

// Wait until fd is ready for `events` or the deadline passes.
bool waitFor(int fd, short events, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return false;
    pollfd p{fd, events, 0};
    int n = ::poll(&p, 1, (int)left);
    if (n > 0) return (p.revents & (events | POLLHUP | POLLERR)) != 0;
    if (n == 0) return false;
    if (errno != EINTR) return false;
  }
}

} // namespace

bool notify(std::string_view state) {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || (path[0] != '/' && path[0] != '@')) return false;
  size_t len = std::strlen(path);
  sockaddr_un addr{};
  if (len >= sizeof(addr.sun_path)) return false;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path, len);
  if (path[0] == '@') addr.sun_path[0] = 0;  // abstract namespace
  // An abstract address is exactly its length; a path one includes the NUL.
  auto addrLen = (socklen_t)(offsetof(sockaddr_un, sun_path) + len + (path[0] == '/' ? 1 : 0));

  Fd sock(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (sock.fd < 0) return false;
  ssize_t sent;
  do {
    sent = ::sendto(sock.fd, state.data(), state.size(), MSG_NOSIGNAL, (const sockaddr*)&addr, addrLen);
  } while (sent < 0 && errno == EINTR);
  return sent == (ssize_t)state.size();
}

std::chrono::microseconds watchdogTimeout() {
  unsigned long long usec = 0;
  if (!parseUnsigned(std::getenv("WATCHDOG_USEC"), usec) || usec == 0) return {};
  if (const char* pid = std::getenv("WATCHDOG_PID")) {
    unsigned long long target = 0;
    if (!parseUnsigned(pid, target) || target != (unsigned long long)::getpid()) return {};
  }
  return std::chrono::microseconds((long long)usec);
}

bool probeHealth(int port, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  Fd sock(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (sock.fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(sock.fd, (const sockaddr*)&addr, sizeof(addr)) != 0) {
    if (errno != EINPROGRESS || !waitFor(sock.fd, POLLOUT, deadline)) return false;
    int err = 0;
    socklen_t errLen = sizeof(err);
    if (::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &err, &errLen) != 0 || err != 0) return false;
  }

  static constexpr std::string_view request =
      "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  size_t off = 0;
  while (off < request.size()) {
    ssize_t n = ::send(sock.fd, request.data() + off, request.size() - off, MSG_NOSIGNAL);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(sock.fd, POLLOUT, deadline)) continue;
    return false;
  }

  // The status line is all that matters: "HTTP/1.1 200 ".
  static constexpr std::string_view ok = "HTTP/1.1 200 ";
  std::string head;
  char buf[64];
  while (head.size() < ok.size()) {
    ssize_t n = ::recv(sock.fd, buf, sizeof(buf), 0);
    if (n > 0) { head.append(buf, (size_t)n); continue; }
    if (n == 0) break;
    if (errno == EINTR) continue;
    if ((errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(sock.fd, POLLIN, deadline)) continue;
    return false;
  }
  return head.starts_with(ok);
}

} // namespace luxir::systemd
