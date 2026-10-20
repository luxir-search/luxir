// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceMerge.h"

#include <algorithm>
#include <cassert>
#include <boost/unordered/unordered_flat_map.hpp>
#include <chrono>

#include "SourceWriter.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

struct FileUse {
  SourceFileRef ref;
  int64_t live = 0;  // records of the file still live in the merged segment
  bool relocate = false;
  std::optional<uint32_t> outIndex;  // index in the merged map, once referenced
  // Relocated files: opened on first use.
  std::shared_ptr<InputFile> input;
  std::optional<source::FileReader> reader;
  bool unreadable = false;
};

class Merger {
public:
  Merger(Directory& dir, const SourceSettings& settings, std::span<const SourceMergeInput> inputs,
         const std::function<std::string()>& newName)
      : dir(dir), settings(settings), inputs(inputs), newName(newName) {}

  SourceMergeResult run() {
    for (const auto& in : inputs) result.docs += liveCount(in, 0, in.maxDoc);
    if (!settings.enabled) return std::move(result);
    countLive();
    decide();
    emit();
    return std::move(result);
  }

private:
  Directory& dir;
  const SourceSettings& settings;
  std::span<const SourceMergeInput> inputs;
  const std::function<std::string()>& newName;
  SourceMergeResult result;
  boost::unordered_flat_map<std::string, FileUse> files;
  std::vector<std::string> order;  // file names in first-use order
  std::unique_ptr<SourceFileSink> sink;
  std::unique_ptr<source::FileWriter> writer;
  std::string writtenName;
  uint32_t writtenIndex = 0;
  source::Codecs codecs;

  static bool isLive(const SourceMergeInput& in, int32_t doc) { return in.live == nullptr || in.live->get(doc); }

  static int32_t liveCount(const SourceMergeInput& in, int32_t from, int32_t to) {
    if (!in.live) return to - from;
    int32_t n = 0;
    for (int32_t d = from; d < to; d++) n += in.live->get(d) ? 1 : 0;
    return n;
  }

  void countLive() {
    for (const auto& in : inputs) {
      if (!in.map) continue;
      for (const auto& run : in.map->runs) {
        const auto& ref = in.map->files[run.file];
        auto [it, inserted] = files.try_emplace(ref.name);
        if (inserted) {
          it->second.ref = ref;
          order.push_back(ref.name);
        }
        it->second.live += liveCount(in, run.docStart, run.docStart + run.count);
      }
    }
  }

  void decide() {
    for (const auto& name : order) {
      auto& use = files.at(name);
      double ratio = use.ref.docs == 0 ? 0.0 : (double)use.live / (double)use.ref.docs;
      use.relocate = ratio < settings.minLiveRatio || use.ref.bytes < settings.minFileBytes;
    }
  }

  void openWriter() {
    if (writer) return;
    writtenName = newName();
    sink = std::make_unique<SourceFileSink>(dir, writtenName);
    auto now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    writer = std::make_unique<source::FileWriter>(*sink, settings.writerOptions(), "merge", now);
    writtenIndex = result.map.fileIndex({writtenName, 0, 0});
  }

  // False if the file cannot be read at all.
  bool open(FileUse& use) {
    if (use.reader) return true;
    if (use.unreadable) return false;
    try {
      use.input = dir.openFile(use.ref.name);
      if (use.input) {
        std::string error;
        use.reader = source::FileReader::open(use.input->read(), &error);
        if (!use.reader) LOG_WARN("source file {} is unreadable: {}", use.ref.name, error);
      } else {
        LOG_WARN("source file {} is missing", use.ref.name);
      }
    } catch (const std::exception& e) {
      LOG_WARN("source file {} could not be opened: {}", use.ref.name, e.what());
    }
    use.unreadable = !use.reader;
    return use.reader.has_value();
  }

  void placeholder() { writer->add({{}, {}, 0, source::kFlagMissing}); }

  // Copies ordinals [from, from + count) of a relocated file to the new file, whose
  // first copied ordinal goes to `firstOut`. A damaged block becomes placeholders,
  // so the rest of the run keeps its ordinals. False (nothing written) if the file
  // cannot be opened.
  bool copy(FileUse& use, uint32_t from, uint32_t count, uint32_t& firstOut) {
    if (!open(use)) return false;
    auto& reader = *use.reader;
    openWriter();
    firstOut = writer->docCount();
    uint32_t end = from + count;
    for (uint32_t ord = from; ord < end;) {
      auto block = reader.blockFor(ord);
      if (block < 0) {
        placeholder();
        result.lostDocs++;
        ord++;
        continue;
      }
      const auto& ref = reader.blocks()[(size_t)block];
      uint32_t blockEnd = ref.firstOrdinal + ref.nDocs;
      uint32_t stop = std::min(end, blockEnd);
      source::FileReader::RawBlock raw;
      if (ord == ref.firstOrdinal && end >= blockEnd && reader.rawBlock((size_t)block, raw) && raw.dictId == 0) {
        writer->addRawBlock(raw.codec, raw.dictId, raw.nDocs, raw.rawLen, raw.payload);
      } else if (source::DecodedBlock decoded; reader.readBlock((size_t)block, codecs, decoded)) {
        for (uint32_t o = ord; o < stop; o++) writer->add(decoded.record(o - ref.firstOrdinal));
      } else {
        for (uint32_t o = ord; o < stop; o++) placeholder();
        result.lostDocs += stop - ord;
      }
      ord = stop;
    }
    return true;
  }

  void emit() {
    int32_t outDoc = 0;
    for (const auto& in : inputs) {
      int32_t at = 0;  // docs before this are accounted for
      if (in.map) {
        for (const auto& run : in.map->runs) {
          outDoc += liveCount(in, at, run.docStart);
          auto& use = files.at(in.map->files[run.file].name);
          int32_t runEnd = run.docStart + run.count;
          for (int32_t doc = run.docStart; doc < runEnd;) {
            if (!isLive(in, doc)) { doc++; continue; }
            int32_t stop = doc;
            while (stop < runEnd && isLive(in, stop)) stop++;
            uint32_t ordinal = run.ordinal + (uint32_t)(doc - run.docStart);
            uint32_t copied = 0;
            if (!use.relocate) {
              if (!use.outIndex) use.outIndex = result.map.fileIndex(use.ref);
              result.map.addRun(outDoc, stop - doc, *use.outIndex, ordinal);
            } else if (copy(use, ordinal, (uint32_t)(stop - doc), copied)) {
              result.map.addRun(outDoc, stop - doc, writtenIndex, copied);
            } else {
              result.lostDocs += stop - doc;
            }
            outDoc += stop - doc;
            doc = stop;
          }
          at = runEnd;
        }
      }
      outDoc += liveCount(in, at, in.maxDoc);
    }
    assert(outDoc == result.docs);
    if (writer) {
      writer->finish();
      uint32_t docs = writer->docCount();
      auto descriptor = sink->finish();
      result.map.files[writtenIndex].docs = docs;
      result.map.files[writtenIndex].bytes = descriptor.size;
      result.written = descriptor;
    }
  }
};

}  // namespace

SourceMergeResult mergeSources(Directory& dir, const SourceSettings& settings,
                               std::span<const SourceMergeInput> inputs,
                               const std::function<std::string()>& newName) {
  return Merger(dir, settings, inputs, newName).run();
}

}  // namespace luxir
