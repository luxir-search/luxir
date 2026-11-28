// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceMerge.h"

#include <algorithm>
#include <bit>
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
  Layout(const SourceSettings& settings, std::span<const SourceMap* const> inputs, SourceDocMap docMap,
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
  SourceDocMap docMap;
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
    if (docMap.size() != inputs.size()) throw std::logic_error("source merge needs one doc mapping per input");
    for (size_t i = 0; i < inputs.size(); i++) {
      const SourceMap* map = inputs[i];
      if (!map) continue;
      const SourceDocMapping& mapping = docMap[i];
      for (const auto& run : map->runs) {
        uint32_t file = fileIndex(map->files[run.file]);
        size_t open = SIZE_MAX;  // the piece the previous doc extended
        int32_t runEnd = run.docStart + run.count;
        for (int32_t doc = run.docStart; doc < runEnd; doc++) {
          int32_t mapped = mapping(doc);
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
      // A patched record's version follows its doc.
      for (const auto& [doc, version] : map->versions) {
        int32_t mapped = mapping(doc);
        if (mapped >= 0) result.map.versions.emplace_back(mapped, version);
      }
    }
    std::ranges::sort(result.map.versions);
  }

  // Estimated bytes of the file's records that are still live.
  static double liveBytesOf(const FileUse& use) {
    return use.ref.docs == 0 ? 0.0 : (double)use.ref.bytes * (double)use.live / (double)use.ref.docs;
  }

  void decide() {
    double liveBytes = 0;
    for (const auto& use : files) liveBytes += liveBytesOf(use);
    double share = settings.maxFiles != 0 ? liveBytes / (double)settings.maxFiles : 0.0;
    for (auto& use : files) {
      if (use.live == 0) continue;
      double ratio = use.ref.docs == 0 ? 0.0 : (double)use.live / (double)use.ref.docs;
      use.relocate = ratio < settings.minLiveRatio || use.ref.bytes < settings.minFileBytes ||
                     (double)use.ref.bytes < share;
    }
    // The cap: at most maxFiles referenced files survive, the ones with the most
    // live bytes, which are the most expensive to copy.  Files at exactly one
    // share each would otherwise all survive.
    std::vector<size_t> kept;
    for (size_t f = 0; f < files.size(); f++) {
      if (files[f].live > 0 && !files[f].relocate) kept.push_back(f);
    }
    if (kept.size() > settings.maxFiles) {
      std::ranges::sort(kept, std::greater<>{}, [&](size_t f) { return liveBytesOf(files[f]); });
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
  FormatRelocation(Directory& dir, const SourceSettings& settings, std::function<std::string()> newName,
                   std::shared_ptr<const StoredCompressionDictionary> dictionary)
      : dir(dir), settings(settings), newName(std::move(newName)), dictionary(std::move(dictionary)),
        dictId(this->dictionary ? source::dictionaryId(this->dictionary->bytes()) : 0) {}

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
      if (!verifiedBlock(file, *reader, (size_t)block)) {
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
      // verify() checked every block a copy reads; its checksum is not computed again.
      const auto* raw = verifiedBlock(file, *reader, (size_t)block);
      // A full block compressed as the output would be is copied as it is; a
      // partial one (a flush's last block), or one in another codec or
      // dictionary, is repacked record by record so that small blocks fold
      // together and old blocks move to the current dictionary.
      if (raw && ord == ref.firstOrdinal && end >= blockEnd && copyable(*raw) && full(*raw)) {
        writer->addRawBlock(raw->codec, raw->dictId, raw->nDocs, raw->rawLen, raw->payload);
      } else if (source::DecodedBlock decoded; raw && reader->decodeBlock((size_t)block, *raw, codecs, decoded)) {
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
    HeldRange hold;  // the whole file, while `reader` views it
    std::optional<source::FileReader> reader;
    // Blocks whose checksum has passed, by index; the payloads point into `input`.
    boost::unordered_flat_map<size_t, source::FileReader::RawBlock> verified;
  };

  Directory& dir;
  const SourceSettings& settings;
  std::function<std::string()> newName;
  boost::unordered_flat_map<std::string, OpenFile> opened;
  // Before the writer, which uses its prepared CDict, so it is destroyed after it.
  std::shared_ptr<const StoredCompressionDictionary> dictionary;
  uint32_t dictId = 0;
  std::unique_ptr<SourceFileSink> sink;
  std::unique_ptr<source::FileWriter> writer;
  std::string name;
  source::Codecs codecs;

  // A block an output block would match: compressed with the current codec and
  // dictionary, or stored uncompressed because compression did not help.
  bool copyable(const source::FileReader::RawBlock& raw) const {
    if (raw.codec == source::Codec::NONE) return raw.dictId == 0;
    return raw.codec == settings.codec && raw.dictId == (settings.codec == source::Codec::ZSTD ? dictId : 0);
  }

  const source::FileReader* open(const SourceFileRef& file) {
    auto [it, inserted] = opened.try_emplace(file.name);
    auto& entry = it->second;
    if (inserted) {
      try {
        entry.input = dir.openFile(file.name, {.size = file.bytes});
        if (!entry.input) {
          LOG_WARN("source file {} is missing", file.name);
        } else {
          std::string error;
          entry.hold = entry.input->acquire({0, entry.input->size()});
          entry.reader = source::FileReader::open(std::string_view(entry.hold.getData(), entry.hold.getLength()), &error);
          if (!entry.reader) LOG_WARN("source file {} is unreadable: {}", file.name, error);
        }
      } catch (const std::exception& e) {
        LOG_WARN("source file {} could not be opened: {}", file.name, e.what());
      }
    }
    return entry.reader ? &*entry.reader : nullptr;
  }

  // Block `block` of an open file, verified once and remembered; null if damaged.
  const source::FileReader::RawBlock* verifiedBlock(const SourceFileRef& file, const source::FileReader& reader,
                                                    size_t block) {
    auto& verified = opened.find(file.name)->second.verified;
    if (auto it = verified.find(block); it != verified.end()) return &it->second;
    source::FileReader::RawBlock raw;
    if (!reader.rawBlock(block, raw)) return nullptr;
    return &verified.emplace(block, raw).first->second;
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
    if (settings.codec == source::Codec::ZSTD) SourceWriter::useDictionary(*writer, dictionary.get());
  }
};

}  // namespace

SourceMergeResult mergeSources(const SourceSettings& settings, std::span<const SourceMap* const> inputs,
                               SourceDocMap docMap, SourceRelocation& relocation) {
  return Layout(settings, inputs, docMap, relocation).run();
}

std::unique_ptr<SourceRelocation> sourceFormatRelocation(
    Directory& dir, const SourceSettings& settings, std::function<std::string()> newName,
    std::shared_ptr<const StoredCompressionDictionary> dictionary) {
  return std::make_unique<FormatRelocation>(dir, settings, std::move(newName), std::move(dictionary));
}

std::string trainSourceDictionary(Directory& dir, const SourceSettings& settings,
                                  std::span<const SourceMap* const> inputs) {
  auto format = settings.dictionaryFormat();
  if (format.codec != StoredChunkFormat::ZSTD || !format.dictionaryBytes) return {};
  size_t budget = StoredChunkCompressor::dictionarySampleBudget(format);
  // Files compress, so their bytes bound what they can contribute only from
  // below; files holding less than a quarter of the budget are not worth opening.
  boost::unordered_flat_map<std::string, uint64_t> names;
  uint64_t fileBytes = 0;
  for (const auto* input : inputs) {
    if (!input) continue;
    for (const auto& ref : input->files) {
      if (names.emplace(ref.name, ref.bytes).second) fileBytes += ref.bytes;
    }
  }
  if (fileBytes * 4 < budget) return {};
  struct Open {
    std::shared_ptr<InputFile> input;
    HeldRange hold;  // the whole file, while `reader` views it
    source::FileReader reader;
  };
  std::vector<Open> files;
  uint64_t blocks = 0;
  for (const auto& [name, bytes] : names) {
    try {
      auto input = dir.openFile(name, {.size = bytes});
      if (!input) continue;
      auto hold = input->acquire({0, input->size()});
      auto reader = source::FileReader::open(std::string_view(hold.getData(), hold.getLength()));
      if (!reader) continue;
      blocks += reader->blocks().size();
      files.push_back({std::move(input), std::move(hold), std::move(*reader)});
    } catch (const std::exception& e) {
      LOG_WARN("source file {} could not be opened for dictionary training: {}", name, e.what());
    }
  }
  if (!blocks) return {};
  std::string samples;
  std::vector<size_t> sizes;
  samples.reserve(budget);
  source::Codecs codecs;
  source::DecodedBlock decoded;
  // Bit-reversed order over the concatenated blocks spreads the sample across
  // files and positions (as the stored-fields bootstrap does).
  uint64_t limit = std::bit_ceil(blocks);
  uint64_t position = 0;
  for (uint64_t visited = 0; visited < limit && samples.size() < budget; visited++) {
    if (position < blocks) {
      uint64_t block = position;
      for (auto& file : files) {
        if (block >= file.reader.blocks().size()) {
          block -= file.reader.blocks().size();
          continue;
        }
        if (file.reader.readBlock((size_t)block, codecs, decoded)) {
          for (uint32_t i = 0; i < decoded.nDocs && samples.size() < budget; i++) {
            auto record = decoded.record(i);
            if (record.body.empty()) continue;
            samples.append(record.body.data(), std::min(record.body.size(), budget - samples.size()));
          }
        }
        break;
      }
    }
    uint64_t bit = limit >> 1;
    while (position & bit) { position ^= bit; bit >>= 1; }
    position ^= bit;
  }
  if (samples.size() < budget) return {};
  // Bounded training blocks, as the stored-fields bootstrap cuts them.
  size_t block = std::max(StoredChunkCompressor::DICTIONARY_SAMPLE_BYTES,
                          (samples.size() + StoredChunkCompressor::MAX_DICTIONARY_SAMPLES - 1) /
                              StoredChunkCompressor::MAX_DICTIONARY_SAMPLES);
  for (size_t offset = 0; offset < samples.size(); offset += block) {
    sizes.push_back(std::min(block, samples.size() - offset));
  }
  size_t capacity = std::max((size_t)1024, std::min((size_t)format.dictionaryBytes, samples.size() / 100));
  return StoredChunkCompressor::trainDictionary(samples, sizes, capacity);
}

}  // namespace luxir
