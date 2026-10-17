// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace luxir {
namespace api { struct AnalyzerComponent; }

// Whole-value cleanup before segmentation. Each chain owns its buffers and
// sparse edits; unchanged input is borrowed. Offsets are UTF-8 byte offsets.
// A replacement's interior maps to its whole source span, so expansions and
// compositions still identify the source that produced a token.
class CharFilter {
  struct Edit { size_t outStart, outEnd, inStart, inEnd; };
  std::string buffer;
  std::vector<Edit> edits;
  size_t copied = 0;
protected:
  std::string_view input;
  virtual void filter() = 0;
  // Non-overlapping replacements in increasing input order. Copies intervening
  // bytes lazily; input and replacement must not alias this filter's buffer.
  void replace(size_t start, size_t end, std::string_view replacement);
public:
  virtual ~CharFilter() = default;
  std::string_view apply(std::string_view value);
  int correctOffset(int offset, bool end) const;
};

class CharFilterFactory {
public:
  std::string name;
  virtual ~CharFilterFactory() = default;
  virtual std::unique_ptr<CharFilter> create() const = 0;
};

std::unique_ptr<const CharFilterFactory> makeCharFilterFactory(const api::AnalyzerComponent& def);
// The existing Unicode cleanup implementations are shared with token filters.
std::unique_ptr<CharFilter> makeUnicodeCharFilter(std::string_view name);
} // namespace luxir
