// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace luxir {

// Buffered writer to a file descriptor that only calls write(2), so a signal
// handler may use it. Text is copied byte by byte; nothing allocates.
class SignalWriter {
  char buf[512];
  size_t n = 0;
  int fd;

public:
  explicit SignalWriter(int fd) : fd(fd) {}
  ~SignalWriter() { flush(); }
  SignalWriter(const SignalWriter&) = delete;
  SignalWriter& operator=(const SignalWriter&) = delete;

  void flush() {
    size_t done = 0;
    while (done < n) {
      ssize_t w = ::write(fd, buf + done, n - done);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) break;
      done += (size_t)w;
    }
    n = 0;
  }

  SignalWriter& ch(char c) {
    if (n == sizeof(buf)) flush();
    buf[n++] = c;
    return *this;
  }

  SignalWriter& str(const char* s) {
    while (*s) ch(*s++);
    return *this;
  }

  // At most `max` bytes of `s`, stopping at a NUL; non-printable bytes become '?'
  // so a field read from an untrusted file cannot inject terminal controls.
  SignalWriter& field(const char* s, size_t max) {
    for (size_t i = 0; i < max && s[i]; i++) ch(s[i] >= ' ' && s[i] < 127 ? s[i] : '?');
    return *this;
  }

  SignalWriter& hex(uintptr_t v) {
    char digits[2 * sizeof(v)];
    int i = 0;
    do {
      digits[i++] = "0123456789abcdef"[v & 15];
      v >>= 4;
    } while (v);
    str("0x");
    while (i) ch(digits[--i]);
    return *this;
  }

  SignalWriter& dec(long long v) {
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    char digits[24];
    int i = 0;
    do {
      digits[i++] = (char)('0' + u % 10);
      u /= 10;
    } while (u);
    if (v < 0) ch('-');
    while (i) ch(digits[--i]);
    return *this;
  }
};

} // namespace luxir
