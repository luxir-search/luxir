// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace luxir::ip {

// An IP address as its 16-byte big-endian IPv6 form. IPv4 addresses map into
// IPv6 as ::ffff:a.b.c.d, so one byte order serves both families and
// lexicographic byte order is address order.
inline constexpr size_t BYTES = 16;
using Address = std::array<uint8_t, BYTES>;

// Longest text form inet_ntop can produce (INET6_ADDRSTRLEN, with the NUL).
inline constexpr size_t TEXT_BUF_SIZE = 46;

inline bool isV4Mapped(const uint8_t* a) {
  static constexpr uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  return std::memcmp(a, prefix, sizeof(prefix)) == 0;
}

// Parse dotted IPv4 or IPv6 text. Zone ids ("fe80::1%eth0") and CIDR
// suffixes are not addresses and return nullopt.
inline std::optional<Address> parse(std::string_view text) {
  char buf[TEXT_BUF_SIZE];
  if (text.empty() || text.size() >= sizeof(buf)) return std::nullopt;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  Address a{};
  if (text.find(':') == std::string_view::npos) {
    if (inet_pton(AF_INET, buf, a.data() + 12) != 1) return std::nullopt;
    a[10] = 0xff;
    a[11] = 0xff;
    return a;
  }
  if (inet_pton(AF_INET6, buf, a.data()) != 1) return std::nullopt;
  return a;
}

// Canonical text: dotted IPv4 for v4-mapped addresses, else the compressed
// lowercase IPv6 form. term must be 16 bytes; returns a view of buf.
inline std::string_view format(std::string_view term, char (&buf)[TEXT_BUF_SIZE]) {
  auto* a = (const uint8_t*)term.data();
  bool v4 = isV4Mapped(a);
  if (inet_ntop(v4 ? AF_INET : AF_INET6, v4 ? a + 12 : a, buf, sizeof(buf)) == nullptr) {
    return {};
  }
  return std::string_view(buf);
}

// The inclusive [lo, hi] address range of a CIDR block ("10.0.0.0/8",
// "2001:db8::/32"). Host bits in the address are ignored. nullopt when text
// has no '/' or does not parse.
struct Range {
  Address lo;
  Address hi;
};

inline std::optional<Range> parseCidr(std::string_view text) {
  size_t slash = text.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  auto addr = parse(text.substr(0, slash));
  std::string_view bitsText = text.substr(slash + 1);
  if (!addr || bitsText.empty() || bitsText.size() > 3) return std::nullopt;
  int bits = 0;
  for (char c : bitsText) {
    if (c < '0' || c > '9') return std::nullopt;
    bits = bits * 10 + (c - '0');
  }
  bool v4 = text.substr(0, slash).find(':') == std::string_view::npos;
  if (bits > (v4 ? 32 : 128)) return std::nullopt;
  if (v4) bits += 96;
  Range r{*addr, *addr};
  for (int i = 0; i < (int)BYTES; i++) {
    int keep = bits - i * 8;
    uint8_t mask = keep >= 8 ? 0xff : keep <= 0 ? 0 : (uint8_t)(0xff << (8 - keep));
    r.lo[i] &= mask;
    r.hi[i] |= (uint8_t)~mask;
  }
  return r;
}

inline std::string_view bytes(const Address& a) {
  return std::string_view((const char*)a.data(), a.size());
}

} // namespace luxir::ip
