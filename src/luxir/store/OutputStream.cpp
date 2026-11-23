// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/OutputStream.h"

#include "luxir/store/AcquireJoin.h"

namespace luxir {

std::vector<HeldRange> InputFile::acquireAll(std::span<const ByteRange> ranges) {
  std::vector<HeldRange> result;
  AcquireJoin join;
  join.acquire(*this, ranges, [&](std::vector<HeldRange>&& holds) { result = std::move(holds); });
  join.wait();
  return result;
}

void File::appendFile(OutputStream& out, RAMFile& file) {
  flush(out, true, 0);
  destructiveAppend(file);
  out.flushedSize = size();
  flush(out, false, 0);
}

void File::borrowBuffer(OutputStream& source, OutputStream& dest, size_t appended) noexcept {
  assert(source.start == nullptr);
  source.flushedSize += appended;
  source.start = source.pos = dest.pos;
  source.end = dest.end;
}

void File::returnBuffer(OutputStream& source, OutputStream& dest) noexcept {
  if (source.start == nullptr) return;
  assert(source.start == dest.pos && source.end == dest.end);
  source.flushedSize += source.buffered();
  dest.pos = source.pos;
  source.start = source.pos = source.end = nullptr;
}

void OutputStream::appendFile(RAMFile& file) {
  target->appendFile(*this, file);
}

} // namespace luxir
