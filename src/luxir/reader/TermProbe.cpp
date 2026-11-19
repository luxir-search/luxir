// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TermProbe.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <map>

#include "SegFieldInfo.h"
#include "SegmentCore.h"

namespace luxir {

void TermProbe::fetch(uint32_t fileNum, std::vector<ByteRange>& ranges) {
  InputFile& file = *reader.core().getFile(fileNum);
  uint64_t size = (uint64_t) file.size();
  for (auto& range : ranges) {
    range.offset = std::min(range.offset, size);
    range.length = std::min(range.length, size - range.offset);
  }
  std::erase_if(ranges, [](const ByteRange& range) { return range.length == 0; });
  if (ranges.empty()) return;
  std::promise<std::exception_ptr> promise;
  auto done = promise.get_future();
  file.fetchRanges(ranges, kNoReadDeadline, [&promise](std::exception_ptr error) { promise.set_value(error); });
  auto error = done.get();
  if (!error) return;
  if (!isFetchNotResident(error)) std::rethrow_exception(error);
  // A file that cannot fetch ahead fetches on first touch.
  for (const auto& range : ranges) file.rangeStream(range.offset, range.length);
}

bool TermProbe::saves(PostingsReader& reader, const SegFieldInfo& field) {
  if (field.nTerms <= 0) return false;
  InputFile& file = *reader.core().getFile(field.termBlockIndexLoc.filenum());
  return file.lazy() && !file.resident(0, (uint64_t) file.size());
}

TermProbe::TermProbe(PostingsReader& reader, const SegFieldInfo& field) : reader(reader), field(field) {
  if (field.nTerms <= 0) return;
  // The block offsets are followed by the trie, whose root node comes last.
  assert(field.termBlockIndexLoc.filenum() == field.trieLoc.filenum());
  uint64_t start = std::min(field.termBlockIndexLoc.offset(), field.trieLoc.offset());
  uint64_t end = field.trieLoc.offset() + (uint64_t) field.trieRootOff + TRIE_ROOT_MAX;
  std::vector<ByteRange> index{{start, end - start}};
  fetch(field.termBlockIndexLoc.filenum(), index);
  if (!field.rangeTableLoc.isNull()) {
    uint32_t tableFile = field.rangeTableLoc.filenum();
    std::vector<ByteRange> header{{field.rangeTableLoc.offset(), sizeof(TermRangeTableHeader)}};
    fetch(tableFile, header);
    TermRangeTableHeader table;
    std::memcpy(&table, reader.core().unfetchedStream(tableFile).ptr(field.rangeTableLoc.offset()), sizeof(table));
    std::vector<ByteRange> rows{{field.rangeTableLoc.offset() + sizeof(TermRangeTableHeader),
                                 (uint64_t) table.nRanges * sizeof(TermRangeRow)}};
    fetch(tableFile, rows);
  }
}

void TermProbe::fetchBlocks(const TermsEnum& terms, std::span<const std::string_view> targets) {
  if (field.nTerms <= 0) return;
  std::map<uint32_t, std::vector<ByteRange>> byFile;
  for (auto target : targets) {
    auto [fileNum, range] = terms.blockRangeFor(target);
    auto& ranges = byFile[fileNum];
    if (ranges.empty() || ranges.back().offset != range.offset) ranges.push_back(range);
  }
  for (auto& [fileNum, ranges] : byFile) fetch(fileNum, ranges);
}

void TermProbe::fetchDocs(TermsEnum& terms) {
  if (auto docs = terms.docsRange()) {
    std::vector<ByteRange> ranges{docs->second};
    fetch(docs->first, ranges);
  }
}

} // namespace luxir
