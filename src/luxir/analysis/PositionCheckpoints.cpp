// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/analysis/PositionCheckpoints.h"

#include <algorithm>
#include <stdexcept>

#include "luxir/util/encoding.h"

namespace luxir {

namespace {
uint32_t readChecked(const char*& p, const char* end) {
  uint32_t value = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (p == end) break;
    auto b = (unsigned char) *p++;
    if (shift == 28 && (b & 0x70)) break;  // more than 32 bits
    value |= (uint32_t) (b & 0x7f) << shift;
    if (!(b & 0x80)) return value;
  }
  throw std::runtime_error("corrupt position checkpoints");
}
}  // namespace

void PositionCheckpoints::encode(std::span<const PositionCheckpoint> list, std::string& out) {
  out.clear();
  char buf[3 * MAX_VINT_SIZE];
  PositionCheckpoint last;
  for (const auto& c : list) {
    char* p = writeVInt(buf, c.value - last.value);
    p = writeVInt(p, (uint32_t) (c.position - last.position));
    p = writeVInt(p, c.value == last.value ? c.offset - last.offset : c.offset);
    out.append(buf, (size_t) (p - buf));
    last = c;
  }
}

void PositionCheckpoints::decode(std::string_view bytes, std::vector<PositionCheckpoint>& out) {
  out.clear();
  const char* p = bytes.data();
  const char* end = p + bytes.size();
  PositionCheckpoint last;
  while (p != end) {
    PositionCheckpoint c;
    uint32_t valueDelta = readChecked(p, end);
    uint32_t positionDelta = readChecked(p, end);
    uint32_t offset = readChecked(p, end);
    c.value = last.value + valueDelta;
    c.position = (int32_t) ((uint32_t) last.position + positionDelta);
    c.offset = valueDelta ? offset : last.offset + offset;
    if (c.value < last.value || c.position < last.position || (!valueDelta && c.offset < last.offset))
      throw std::runtime_error("corrupt position checkpoints");
    out.push_back(c);
    last = c;
  }
}

const PositionCheckpoint* PositionCheckpoints::atOrBefore(std::span<const PositionCheckpoint> list,
                                                          int32_t position) {
  auto it = std::upper_bound(list.begin(), list.end(), position,
      [](int32_t p, const PositionCheckpoint& c) { return p < c.position; });
  return it == list.begin() ? nullptr : &*(it - 1);
}

const PositionCheckpoint* PositionCheckpoints::after(std::span<const PositionCheckpoint> list, int32_t position) {
  auto it = std::upper_bound(list.begin(), list.end(), position,
      [](int32_t p, const PositionCheckpoint& c) { return p < c.position; });
  return it == list.end() ? nullptr : &*it;
}

void FieldTokenStream::start() {
  value = 0;
  position = -1;
  origin = 0;
  stopValue = UINT32_MAX;
  pin = open = done = false;
}

void FieldTokenStream::start(const PositionCheckpoint& from) {
  if (from.value >= values.size() || from.offset > values[from.value].size())
    throw std::runtime_error("position checkpoint lies outside the stored values");
  value = from.value;
  origin = from.offset;
  pinned = from.position;
  position = from.position - 1;
  stopValue = UINT32_MAX;
  pin = true;
  open = done = false;
}

void FieldTokenStream::stopAt(const PositionCheckpoint& until) {
  if (open) throw std::logic_error("FieldTokenStream::stopAt after next()");
  stopValue = until.value;
  stopOffset = until.offset;
  if (until.value < value || (until.value == value && until.offset <= origin)) done = true;
}

void FieldTokenStream::openValue() {
  std::string_view text = values[value];
  size_t end = value == stopValue ? std::min(stopOffset, text.size()) : text.size();
  size_t begin = std::min(origin, end);
  chain.setValue(text.substr(begin, end - begin));
  chain.reset();
  open = true;
}

bool FieldTokenStream::next() {
  while (!done) {
    if (!open) {
      if (value >= values.size() || value > stopValue) break;
      openValue();
    }
    if (chain.tail->incrementToken()) {
      const Token& t = chain.getToken();
      if (pin) {
        position = pinned;
        pin = false;
      } else {
        position += t.positionIncrement;
      }
      startOffset = (int) origin + chain.sourceOffset(t.startOffset, false);
      endOffset = (int) origin + chain.sourceOffset(t.endOffset, true);
      return true;
    }
    if (value == stopValue) break;
    // As FullTextHandler::indexValues: trailing holes, then the value gap.
    position += chain.end();
    open = false;
    origin = 0;
    if (++value < values.size()) position += POSITION_INCREMENT_GAP;
  }
  done = true;
  open = false;
  return false;
}

}  // namespace luxir
