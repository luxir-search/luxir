// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceMerge.h"

#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <chrono>
#include <functional>
#include <stdexcept>

#include "SourceWriter.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

// Consecutive docs of one input run that land on consecutive merged docs.
struct Piece {
  int32_t outStart;
  int32_t count;
  uint32_t file;  // index into Layout::files
  uint32_t ordinal;
};

struct FileUse {
  SourceFileRef ref;
  int64_t live = 0;  // records of the file still live in the merged segment
  bool relocate = false;
  std::optional<uint32_t> outIndex;  // index in the merged map, once referenced
};

// The layout: which files the merged segment references, which it has copied,
// and the runs that point its docs at them.  Records are opaque here.
class Layout {
public:
  Layout(const SourceSettings& settings, std::span<const SourceMap* const> inputs, const SourceDocMap& docMap,
         SourceRelocation& relocation)
      : settings(settings), inputs(inputs), docMap(docMap), relocation(relocation) {}

  SourceMergeResult run() {
    collect();
    if (pieces.empty()) return std::move(result);
    decide();
    emit();
    return std::move(result);
  }

private:
  const SourceSettings& settings;
  std::span<const SourceMap* const> inputs;
  const SourceDocMap& docMap;
  SourceRelocation& relocation;
  SourceMergeResult result;
  std::vector<FileUse> files;  // in first-use order
  boost::unordered_flat_map<std::string, uint32_t> fileIndexByName;
  std::vector<Piece> pieces;   // in input order

  uint32_t fileIndex(const SourceFileRef& ref) {
    auto [it, inserted] = fileIndexByName.try_emplace(ref.name, (uint32_t)files.size());
    if (inserted) {
      FileUse use;
      use.ref = ref;
      files.push_back(std::move(use));
    }
    return it->second;
  }

  void collect() {
    for (size_t i = 0; i < inputs.size(); i++) {
      const SourceMap* map = inputs[i];
      if (!map) continue;
      for (const auto& run : map->runs) {
        uint32_t file = fileIndex(map->files[run.file]);
        size_t open = SIZE_MAX;  // the piece the previous doc extended
        int32_t runEnd = run.docStart + run.count;
        for (int32_t doc = run.docStart; doc < runEnd; doc++) {
          int32_t mapped = docMap(i, doc);
          if (mapped < 0) {
            open = SIZE_MAX;
            continue;
          }
          files[file].live++;
          if (open != SIZE_MAX && pieces[open].outStart + pieces[open].count == mapped) {
            pieces[open].count++;
            continue;
          }
          pieces.push_back({mapped, 1, file, run.ordinal + (uint32_t)(doc - run.docStart)});
          open = pieces.size() - 1;
        }
      }
    }
  }

  void decide() {
    double liveBytes = 0;
    for (const auto& use : files) {
      if (use.ref.docs != 0) liveBytes += (double)use.ref.bytes * (double)use.live / (double)use.ref.docs;
    }
    double share = settings.maxFiles != 0 ? liveBytes / (double)settings.maxFiles : 0.0;
    for (auto& use : files) {
      if (use.live == 0) continue;
      double ratio = use.ref.docs == 0 ? 0.0 : (double)use.live / (double)use.ref.docs;
      use.relocate = ratio < settings.minLiveRatio || use.ref.bytes < settings.minFileBytes ||
                     (double)use.ref.bytes < share;
    }
    // The cap: at most maxFiles referenced files survive, the largest ones.  Files
    // at exactly one share each would otherwise all survive.
    std::vector<size_t> kept;
    for (size_t f = 0; f < files.size(); f++) {
      if (files[f].live > 0 && !files[f].relocate) kept.push_back(f);
    }
    if (kept.size() > settings.maxFiles) {
      std::ranges::sort(kept, std::greater<>{}, [&](size_t f) { return files[f].ref.bytes; });
      for (size_t k = settings.maxFiles; k < kept.size(); k++) files[kept[k]].relocate = true;
    }
    // A file the relocation cannot read stays referenced as it is: the merge
    // then loses nothing that was not already lost.
    for (size_t f = 0; f < files.size(); f++) {
      auto& use = files[f];
      if (!use.relocate) continue;
      for (const auto& piece : pieces) {
        if (piece.file != f) continue;
        if (relocation.verify(use.ref, piece.ordinal, (uint32_t)piece.count)) continue;
        use.relocate = false;
        result.unreadable.push_back(use.ref.name);
        break;
      }
    }
  }

  void emit() {
    std::optional<uint32_t> writtenIndex;
    for (const auto& piece : pieces) {
      auto& use = files[piece.file];
      if (!use.relocate) {
        if (!use.outIndex) use.outIndex = result.map.fileIndex(use.ref);
        result.map.addRun(piece.outStart, piece.count, *use.outIndex, piece.ordinal);
        continue;
      }
      uint32_t firstOut = 0;
      relocation.copy(use.ref, piece.ordinal, (uint32_t)piece.count, firstOut);
      if (!writtenIndex) writtenIndex = result.map.fileIndex({relocation.outputName(), 0, 0});
      result.map.addRun(piece.outStart, piece.count, *writtenIndex, firstOut);
    }
    if (writtenIndex) {
      auto output = relocation.finish();
      if (!output) throw std::logic_error("source relocation copied records but produced no file");
      result.map.files[*writtenIndex] = output->ref;
      result.written = output->descriptor;
    }
    result.map.normalize();
  }
};

// Relocation over source-format files.
class FormatRelocation final : public SourceRelocation {
public:
  FormatRelocation(Directory& dir, const SourceSettings& settings, std::function<std::string()> newName)
      : dir(dir), settings(settings), newName(std::move(newName)) {}

  bool verify(const SourceFileRef& file, uint32_t from, uint32_t count) override {
    const auto* reader = open(file);
    if (!reader) return false;
    int64_t block = -1;
    for (uint32_t ord = from, end = from + count; ord < end;) {
      block = reader->blockFor(ord, block);
      if (block < 0) {
        LOG_WARN("source file {} has no block for ordinal {}", file.name, ord);
        return false;
      }
      source::FileReader::RawBlock raw;
      if (!reader->rawBlock((size_t)block, raw)) {
        LOG_WARN("source block {} of {} is damaged", block, file.name);
        return false;
      }
      const auto& ref = reader->blocks()[(size_t)block];
      ord = ref.firstOrdinal + ref.nDocs;
    }
    return true;
  }

  void copy(const SourceFileRef& file, uint32_t from, uint32_t count, uint32_t& firstOut) override {
    const auto* reader = open(file);
    if (!reader) throw std::runtime_error("source file " + file.name + " became unreadable during the merge");
    openWriter();
    firstOut = writer->docCount();
    int64_t block = -1;
    for (uint32_t ord = from, end = from + count; ord < end;) {
      block = reader->blockFor(ord, block);
      if (block < 0) throw std::runtime_error("source file " + file.name + " lost the block of ordinal " + std::to_string(ord));
      const auto& ref = reader->blocks()[(size_t)block];
      uint32_t blockEnd = ref.firstOrdinal + ref.nDocs;
      uint32_t stop = std::min(end, blockEnd);
      source::FileReader::RawBlock raw;
      // A full block is copied as it is; a partial one (a flush's last block)
      // is repacked record by record so that small blocks fold together.
      if (ord == ref.firstOrdinal && end >= blockEnd && reader->rawBlock((size_t)block, raw) && raw.dictId == 0 &&
          full(raw)) {
        writer->addRawBlock(raw.codec, raw.dictId, raw.nDocs, raw.rawLen, raw.payload);
      } else if (source::DecodedBlock decoded; reader->readBlock((size_t)block, codecs, decoded)) {
        for (uint32_t o = ord; o < stop; o++) writer->add(decoded.record(o - ref.firstOrdinal));
      } else {
        throw std::runtime_error("source block " + std::to_string(block) + " of " + file.name +
                                 " became unreadable during the merge");
      }
      ord = stop;
    }
  }

  const std::string& outputName() const override { return name; }

  std::optional<Output> finish() override {
    if (!writer) return std::nullopt;
    writer->finish();
    uint32_t docs = writer->docCount();
    auto descriptor = sink->finish();
    writer.reset();
    sink.reset();
    return Output{{name, docs, descriptor.size}, descriptor};
  }

private:
  struct OpenFile {
    std::shared_ptr<InputFile> input;
    std::optional<source::FileReader> reader;
  };

  Directory& dir;
  const SourceSettings& settings;
  std::function<std::string()> newName;
  boost::unordered_flat_map<std::string, OpenFile> opened;
  std::unique_ptr<SourceFileSink> sink;
  std::unique_ptr<source::FileWriter> writer;
  std::string name;
  source::Codecs codecs;

  const source::FileReader* open(const SourceFileRef& file) {
    auto [it, inserted] = opened.try_emplace(file.name);
    auto& entry = it->second;
    if (inserted) {
      try {
        entry.input = dir.openFile(file.name);
        if (!entry.input) {
          LOG_WARN("source file {} is missing", file.name);
        } else {
          std::string error;
          entry.reader = source::FileReader::open(entry.input->read(), &error);
          if (!entry.reader) LOG_WARN("source file {} is unreadable: {}", file.name, error);
        }
      } catch (const std::exception& e) {
        LOG_WARN("source file {} could not be opened: {}", file.name, e.what());
      }
    }
    return entry.reader ? &*entry.reader : nullptr;
  }

  bool full(const source::FileReader::RawBlock& raw) const {
    return raw.rawLen >= settings.blockBytes ||
           (settings.maxDocsPerBlock != 0 && raw.nDocs >= settings.maxDocsPerBlock);
  }

  void openWriter() {
    if (writer) return;
    name = newName();
    sink = std::make_unique<SourceFileSink>(dir, name);
    auto now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    writer = std::make_unique<source::FileWriter>(*sink, settings.writerOptions(), "merge", now);
  }
};

}  // namespace

SourceMergeResult mergeSources(const SourceSettings& settings, std::span<const SourceMap* const> inputs,
                               const SourceDocMap& docMap, SourceRelocation& relocation) {
  return Layout(settings, inputs, docMap, relocation).run();
}

std::unique_ptr<SourceRelocation> sourceFormatRelocation(Directory& dir, const SourceSettings& settings,
                                                         std::function<std::string()> newName) {
  return std::make_unique<FormatRelocation>(dir, settings, std::move(newName));
}

}  // namespace luxir
