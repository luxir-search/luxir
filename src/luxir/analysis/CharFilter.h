// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxir {
namespace api { struct AnalyzerComponent; }

// Whole-value cleanup before segmentation. Each chain owns its buffers and
// sparse edits; unchanged input is borrowed. Offsets are UTF-8 byte offsets.
// A replacement's interior maps to its whole source span, so expansions and
// compositions still identify the source that produced a token.
class CharFilter {
  struct Edit { size_t outStart, outEnd, inStart, inEnd; };
  struct Cursor { size_t index = 0; int offset = 0; };
  std::string buffer;
  std::vector<Edit> edits;
  std::array<Cursor, 2> cursors{};
  size_t copied = 0;
  // Input spans the scan consumed whole without an edit (an identity
  // mapping, CDATA): resuming inside one would rescan it.
  std::vector<std::pair<size_t, size_t>> opaque;
protected:
  std::string_view input;
  virtual void filter() = 0;
  // Non-overlapping replacements in increasing input order. Copies intervening
  // bytes lazily; input and replacement must not alias this filter's buffer.
  void replace(size_t start, size_t end, std::string_view replacement);
  // Marks [start, end) as handled in one step: the scan never visits its
  // interior, whether or not the span was edited. Only resumable filters
  // need to call it, for spans they skip without an edit.
  void consumed(size_t start, size_t end);
  // For a filter whose scan carries no state between scan positions: true
  // when `offset`, outside every replaced or consumed span, is such a scan
  // position. Then the output from `offset` depends only on the input from
  // there, and the output before it only on the input before it (so a
  // reader may also cut the input at such a position). Default no:
  // resumesAt() never holds.
  virtual bool scanPosition(size_t /*offset*/) const { return false; }
  // Fill every byte of a same-length result with identity byte offsets.
  // Call before any replacements; the span cannot resize the output buffer.
  std::span<char> bytewiseOutput();
public:
  virtual ~CharFilter() = default;
  std::string_view apply(std::string_view value);
  // Start/end searches advance independently; decreasing requests rewind.
  int correctOffset(int offset, bool end);
  // After apply(): whether apply(value.substr(offset)) equals the last
  // output from outputOffset(offset) on, with the same edits shifted.
  bool resumesAt(size_t offset) const;
  // After apply(): where input `offset`, outside every replaced span, lands in
  // the output.
  size_t outputOffset(size_t offset) const;
};

class CharFilterFactory {
public:
  std::string name;
  bool normalizing = false;
  bool resumable = false;  // filters implement scanPosition (see CharFilter::resumesAt)
  virtual ~CharFilterFactory() = default;
  virtual std::unique_ptr<CharFilter> create() const = 0;
};

std::unique_ptr<const CharFilterFactory> makeCharFilterFactory(const api::AnalyzerComponent& def);
std::unique_ptr<CharFilter> makeNfkcCfCharFilter();
} // namespace luxir
