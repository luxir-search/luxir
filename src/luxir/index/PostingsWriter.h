// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <assert.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "luxir/index/TrieBuilder.h"
#include "luxir/index/IdRange.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"
#include "luxir/store/OutputStream.h"
#include "luxir/store/Directory.h"
#include "luxir/codec/StreamVByte.h"
#include "luxir/reader/PostingsReader.h"
#include "IndexFieldInfo.h"
#include "luxir/reader/SegmentCatalog.h"
#include "luxir/reader/CatalogRange.h"
#include "luxir/reader/IdFilter.h"
#include "luxir/reader/Space.h"
#include "ScreamingBuilder.h"
#include "luxir/codec/Codec.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/screaming.h"
#include "LiveDocsWriter.h"
#include "VersionRunBuilder.h"


namespace luxir {

static_assert(kReadSlack == SVB_OVERREAD_PAD);

struct WrittenPoints {
  field_parts::Points points;
  field_parts::Bounds bounds;
};

// Writes a segment's postings, columns, row spaces and catalog.
// An inverter owns its writer. Merge tasks use independent stream checkouts.
class PostingsWriter {
public:
  static constexpr size_t RAM_SPILL_BYTES = 4ULL << 20;

  using IndexFieldInfo = luxir::IndexFieldInfo;

  // The id field's bounds, captured when its dictionary is finalized. Absent
  // when no finalized dictionary was written for it; callers then derive them.
  std::optional<IdRange> idRange;
  std::optional<uint64_t> idDictionaryBlocks;
  // The root id dictionary's membership filter. Set before writing.
  IdFilter::Options idFilterOptions;

private:
  Directory& directory;
  std::vector<RowSpace> spaces;
  CatalogRange catalog;
  int64_t sizeInBytes = 0; // total size of all files written
  // Immutable request births, referenced from segment metadata rather than a field.
  VersionRunsDescriptor versions;
  bool finished = false;

  std::string segStr;

  std::function<void(int64_t)> onStagedRamDelta;

  struct DataFile {
    std::unique_ptr<File> file;
    OutputStream out;
    uint32_t fileNum;
    bool available = false;

    DataFile(Directory& dir, std::string_view segStr, uint32_t fileNum)
        : fileNum(fileNum) {
      auto name = Postings::getIndexFileName(segStr, fileNum);
      file = dir.createFile(name);
      if (!file) throw FileIOException("Failed to create segment file: " + name);
      out.setFile(file.get());
      out.writeBytes(Postings::LUXIR_HEADER);
    }
  };

  std::mutex primaryMutex;
  std::optional<DataFile> primary;
  std::mutex spillMutex;
  std::vector<std::unique_ptr<DataFile>> spillFiles;  // Null slots are reserved for creation.
  std::deque<IndexFieldInfo> fieldInfos;
  std::mutex mutex;  // Field registry and its pool.
public:
  MemPool pool;  // during concurrent writing, access only under mutex (see copyTerm)
  uint64_t segId;
  uint64_t creatorEpoch;

public:
  PostingsWriter(Directory& dir, uint64_t segId, uint64_t creatorEpoch, int32_t maxDoc = -1)
      : directory(dir), spaces{{"", maxDoc, {}}}, segStr(Postings::segmentKey(segId, creatorEpoch)),
        segId(segId), creatorEpoch(creatorEpoch) {}

  // The owner installs its accounting before writing any checkouts. Merge
  // checkouts may call it concurrently.
  void setStagedRamAccounting(std::function<void(int64_t)> account) {
    onStagedRamDelta = std::move(account);
  }

  Directory& getDirectory() {
    return directory;
  }

  // Thread-safe; fields may be registered concurrently and in any order
  // (finish() sorts each space's field table by name).
  IndexFieldInfo& addField(SpaceOrdinal space, PackedTerm fieldName) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (!findSpace(space)) throw std::invalid_argument("field references an undeclared row space");
    fieldInfos.emplace_back();
    fieldInfos.back().space = space;
    fieldInfos.back().header.name = fieldName;
    return fieldInfos.back();
  }

  // copies the fieldName into the pool associated with this PostingsWriter.
  IndexFieldInfo& addField(SpaceOrdinal space, std::string_view fieldName) {
    return addField(space, copyTerm(fieldName));
  }

  void addStoredField(SpaceOrdinal space, std::string_view name, int32_t maxDoc,
                      const field_parts::Stored& stored) {
    auto& field = addField(space, name);
    field.header.type = FieldType::BIN;
    field.header.flags = FieldType::STORED;
    field.setPresence({maxDoc, {}});
    field.setStored(stored);
  }

  // Thread-safe copy of a term into this writer's pool; the copy lives until the
  // writer is destroyed.
  PackedTerm copyTerm(std::string_view s) {
    const std::lock_guard<std::mutex> lock(mutex);
    return PackedTerm(pool, s);
  }

  std::span<const field_parts::TermRun> copyTermRuns(std::span<const field_parts::TermRun> runs) {
    using field_parts::TermRun;
    const std::lock_guard<std::mutex> lock(mutex);
    auto* copy = (TermRun*)pool.alloc(runs.size_bytes(), alignof(TermRun));
    std::uninitialized_copy(runs.begin(), runs.end(), copy);
    return {copy, runs.size()};
  }

  // Keep the endpoints in this writer's pool until the catalog is encoded.
  field_parts::DictionaryBounds dictionaryBounds(std::string_view first, std::string_view last,
                                                  FieldType::Type type) {
    using field_parts::DictionaryBounds;
    using Lower = DictionaryBounds::Lower;
    using Upper = DictionaryBounds::Upper;
    const auto cap = FieldType::dictionaryBoundsCap(type);
    const std::lock_guard<std::mutex> lock(mutex);
    PackedTerm lower(pool, first.substr(0, cap));
    auto lowerMode = first.size() > cap ? Lower::ROUNDED : Lower::EXACT;
    if (last.size() <= cap) return {lower, PackedTerm(pool, last), lowerMode, Upper::INCLUSIVE};

    auto prefix = last.substr(0, cap);
    while (!prefix.empty() && (unsigned char)prefix.back() == UCHAR_MAX) prefix.remove_suffix(1);
    if (prefix.empty()) return {lower, PackedTerm(nullptr), lowerMode, Upper::UNBOUNDED};
    PackedTerm::TermBuffer successor;
    std::memcpy(successor.data(), prefix.data(), prefix.size());
    successor[prefix.size() - 1] = (char)((unsigned char)prefix.back() + 1);
    PackedTerm upper(pool, {successor.data(), prefix.size()});
    return {lower, upper, lowerMode, Upper::EXCLUSIVE};
  }


private:
  DataFile& primaryFile() {
    if (!primary) primary.emplace(directory, segStr, Postings::PRIMARY_FILE);
    return *primary;
  }

  static uint64_t alignedTail(const OutputStream& out) {
    return alignUp(out.size(), MAX_ALIGN);
  }

  static bool fits(uint64_t base, uint64_t bytes) {
    return base <= seg_location::maxOffset() && bytes <= seg_location::maxOffset() - base;
  }

  static void checkSliceSize(uint64_t base, uint64_t bytes) {
    if (!fits(base, bytes)) throw std::length_error("segment offset exceeds seg_location");
  }

  static seg_location beginSlice(DataFile& file, size_t bytes) {
    checkSliceSize(alignedTail(file.out), bytes);
    file.out.align(MAX_ALIGN);
    return {file.fileNum, file.out.size()};
  }

  // Hands out the shortest spill file no stream is writing; a new file is created only
  // when every existing one is checked out, so a segment has at most as many spill
  // files as streams that spilled at the same time.
  DataFile& claimSpillFile(size_t bytes) {
    uint32_t number;
    {
      const std::lock_guard lock(spillMutex);
      DataFile* shortest = nullptr;
      for (const auto& file : spillFiles) {
        if (file && file->available && fits(alignedTail(file->out), bytes)
            && (!shortest || file->out.size() < shortest->out.size())) {
          shortest = file.get();
        }
      }
      if (shortest) {
        shortest->available = false;
        return *shortest;
      }
      if (spillFiles.size() == seg_location::maxFilenum()) {
        throw std::length_error("too many segment files");
      }
      spillFiles.emplace_back();
      number = (uint32_t)spillFiles.size();
    }

    auto file = std::make_unique<DataFile>(directory, segStr, number);
    const std::lock_guard lock(spillMutex);
    return *(spillFiles[number - 1] = std::move(file));
  }

  class StagedStream : private RAMFile {
    friend class PostingsWriter;
    PostingsWriter& writer;
    OutputStream out;
    DataFile* spillFile = nullptr;
    seg_location base;
    size_t chargedBytes = 0;

    void accountCapacity(size_t bytes) noexcept {
      if (writer.onStagedRamDelta && bytes != chargedBytes) {
        writer.onStagedRamDelta((int64_t)bytes - (int64_t)chargedBytes);
      }
      chargedBytes = bytes;
    }

    void spill(size_t additionalBytes) {
      RAMFile::flushBuffer(out, 0);
      checkSliceSize(size(), additionalBytes);
      auto bytes = size() + additionalBytes;
      spillFile = &writer.claimSpillFile(bytes);
      base = beginSlice(*spillFile, bytes);
      if (size() != 0) spillFile->out.appendFile(*this);
      else clear();
      accountCapacity(0);
    }

    void flush(OutputStream& stream, bool deferNewBuffer, size_t minCapacity) override {
      if (spillFile) {
        auto& physical = spillFile->out;
        returnBuffer(stream, physical);
        if (minCapacity != 0) physical.reserve((uint32_t)minCapacity);
        else physical.flush(deferNewBuffer);
        if (!deferNewBuffer) borrowBuffer(stream, physical);
        return;
      }

      if (deferNewBuffer) {
        RAMFile::flushBuffer(stream, 0);
        return;
      }
      size_t needed = std::max(minCapacity, size_t{1});
      size_t remaining = RAM_SPILL_BYTES - allocatedBytes();
      if (needed > remaining) {
        spill(needed);
        auto& physical = spillFile->out;
        physical.reserve((uint32_t)needed);
        borrowBuffer(stream, physical);
      } else {
        RAMFile::flushBuffer(stream, std::min(nextBufferCapacity(stream, needed), remaining));
        accountCapacity(allocatedBytes());
      }
    }

    void close(OutputStream& stream) override {
      flush(stream, true, 0);
    }

    void appendFile(OutputStream& stream, RAMFile& file) override {
      size_t bytes = file.size();
      if (!spillFile && bytes > stream.reserved()) {
        if (bytes > RAM_SPILL_BYTES - allocatedBytes()) spill(bytes);
        else flush(stream, false, bytes);
      }
      if (spillFile) {
        checkSliceSize(base.offset(), stream.size());
        checkSliceSize(base.offset() + stream.size(), bytes);
        auto& physical = spillFile->out;
        returnBuffer(stream, physical);
        physical.appendFile(file);
        borrowBuffer(stream, physical, bytes);
      } else {
        file.appendTo(stream);
        file.clear();
      }
    }

    field_parts::Slice finish() {
      uint64_t bytes = out.size();
      if (spillFile) {
        checkSliceSize(base.offset(), bytes);
        returnBuffer(out, spillFile->out);
        const std::lock_guard lock(writer.spillMutex);
        spillFile->available = true;
        return {base, bytes};
      }
      RAMFile::flushBuffer(out, 0);
      assert(size() == bytes);
      const std::lock_guard lock(writer.primaryMutex);
      auto& primary = writer.primaryFile();
      auto location = beginSlice(primary, bytes);
      appendTo(primary.out);
      return {location, bytes};
    }

  public:
    explicit StagedStream(PostingsWriter& writer) : RAMFile(""), writer(writer), out(this) {}

    // A spilled file stays unavailable on abandon. The failed segment is
    // discarded without flushing any of its remaining buffered writes.
    ~StagedStream() override {
      clear();
      accountCapacity(0);
    }

    OutputStream& output() noexcept { return out; }
  };

public:
  class OutputStreamPtr {
    friend class PostingsWriter;
    std::unique_ptr<StagedStream> stream;

    explicit OutputStreamPtr(PostingsWriter& writer)
        : stream(std::make_unique<StagedStream>(writer)) {}

  public:
    OutputStreamPtr() = default;
    explicit operator bool() const noexcept { return stream != nullptr; }
    OutputStream& operator*() const noexcept { return stream->output(); }
    OutputStream* operator->() const noexcept { return &stream->output(); }
    uint64_t pos() const noexcept { return stream->output().size(); }
  };

  field_parts::Slice finishStream(OutputStreamPtr&& checkout) {
    assert(checkout.stream && &checkout.stream->writer == this);
    auto stream = std::move(checkout.stream);
    return stream->finish();
  }

  // A term or posting stream needs a file extent only if it contains bytes.
  field_parts::Slice finishNonemptyStream(OutputStreamPtr&& checkout) {
    assert(checkout.stream && &checkout.stream->writer == this);
    if (checkout.pos()) return finishStream(std::move(checkout));
    checkout.stream.reset();
    return {};
  }

  void writeVersionRuns(const VersionRunBuilder& builder) {
    if (!spaces.front().maxDoc) return;
    auto stream = getOutputStream();
    VersionRunsOutput output(*stream, (uint32_t)((builder.size() + VersionRuns::BLOCK_SIZE - 1) / VersionRuns::BLOCK_SIZE));
    auto count = builder.finish(output, (uint32_t)spaces.front().maxDoc);
    auto trailerOff = output.finish(count);
    versions = VersionRunsDescriptor{finishStream(std::move(stream)), trailerOff, count};
  }

  class VersionRunsWriter {
    PostingsWriter& writer;
    OutputStreamPtr stream;
    std::optional<VersionRunsOutput> output;
    VersionRunBuilder builder;
    uint32_t lastStart = 0;
    bool added = false;

    VersionRunsOutput& target() {
      if (!output) {
        stream = writer.getOutputStream();
        output.emplace(*stream);
      }
      return *output;
    }

  public:
    explicit VersionRunsWriter(PostingsWriter& writer) : writer(writer),
        builder([this](std::span<const char> block, uint32_t first) { target().add(block, first); }) {}
    VersionRunsWriter(VersionRunsWriter&&) = delete;

    void add(uint32_t firstDocId, uint64_t version) {
      if (added ? firstDocId <= lastStart : firstDocId != 0) {
        throw std::runtime_error("Request-version runs must start at zero and increase");
      }
      builder.add(firstDocId, version);
      lastStart = firstDocId;
      added = true;
    }

    void finish() {
      if (added) {
        auto count = builder.finish(target(), (uint32_t)writer.spaces.front().maxDoc);
        auto trailerOff = target().finish(count);
        output.reset();
        writer.versions = VersionRunsDescriptor{writer.finishStream(std::move(stream)), trailerOff, count};
      }
    }
  };

  OutputStreamPtr getOutputStream() {
    return OutputStreamPtr(*this);
  }

  template <std::size_t N>
  std::array<OutputStreamPtr, N> getOutputStreams() {
    std::array<OutputStreamPtr, N> streams;
    for (auto& stream : streams) stream = getOutputStream();
    return streams;
  }

  uint64_t getSegId() const {
    return segId;
  }

  // returns true if anything was written.
  // If filenames is non-null, appends the names of files written (for fsync at commit time).
  bool finish(std::vector<std::string>* filenames = nullptr, std::vector<FileDescriptor>* descriptors = nullptr) {

    if (finished) return false; // already called
    auto maxDoc = spaces.front().maxDoc;
    for (size_t index = 1; index < spaces.size(); index++) {
      const auto& space = spaces[index];
      if (!space.ownership.ends || space.maxDoc <= 0) {
        throw std::invalid_argument("unfinished row space: " + std::string(space.path));
      }
      auto parent = spaceParentPath(space.path);
      auto entry = std::find_if(spaces.begin(), spaces.end(),
          [&](const auto& candidate) { return candidate.path == parent; });
      if (entry == spaces.end() || entry->maxDoc <= 0) {
        throw std::invalid_argument("row space has no parent rows: " + std::string(space.path));
      }
    }
    if (fieldInfos.empty() && maxDoc <= 0) return false; // no data added

    if (!validSpaceDocCount(maxDoc)) throw std::invalid_argument("root doc count out of range");
    if (maxDoc > 0 && (!versions.slice || versions.count == 0)) {
      throw std::runtime_error("Segment has no request birth versions");
    }

    for (const auto& file : spillFiles) {
      if (!file || !file->available) throw std::logic_error("unfinished spilled checkout");
    }
    primaryFile();
    writeCatalog();

    // The StreamVByte AVX tail decoder used by postings iteration, and the
    // LinearPack bulk decoder used by ord columns, may read up to
    // SVB_OVERREAD_PAD bytes past the encoded data. Reserve that slack at the end of
    // every pure-data file so the read stays in bounds (ord columns write no pad
    // of their own). The catalog supplies this slack in file 0, and its
    // directory length must be the last bytes of that file.
    static const char svbPad[SVB_OVERREAD_PAD] = {};
    for (auto& file : spillFiles) file->out.write(svbPad, SVB_OVERREAD_PAD);

    if (filenames) filenames->reserve(filenames->size() + 1 + spillFiles.size());
    auto finishFile = [&](DataFile& dataFile) {
      sizeInBytes += dataFile.out.size();
      dataFile.out.close();
      directory.finishFile(*dataFile.file);
      if (descriptors) descriptors->push_back(dataFile.file->descriptor());
      if (filenames) filenames->emplace_back(dataFile.file->name());
    };
    finishFile(*primary);
    for (auto& file : spillFiles) finishFile(*file);

    finished = true;
    fieldInfos.clear();
    spillFiles.clear();
    primary.reset();
    return true;
  }

  void setMaxDoc(int max) {
    if (!validSpaceDocCount(max)) throw std::invalid_argument("root doc count out of range");
    spaces.front().maxDoc = max;
  }

  int32_t getMaxDoc(SpaceOrdinal space) const {
    const auto* descriptor = findSpace(space);
    if (!descriptor) throw std::invalid_argument("unknown row space");
    if (space != SpaceOrdinal::ROOT && !descriptor->ownership.ends) throw std::invalid_argument("row space count is not finalized");
    return descriptor->maxDoc;
  }

  int32_t getMaxSpaceRows() const {
    int32_t count = spaces.front().maxDoc;
    for (const auto& space : spaces) count = std::max(count, space.maxDoc);
    return count;
  }

  // Declare a child space before its fields. Ownership streams use the same
  // checkout mechanism as field data.
  SpaceOrdinal addSpace(std::string_view path) {
    auto parent = spaceParentPath(path);
    bool hasParent = false;
    for (const auto& space : spaces) {
      if (space.path == path) throw std::invalid_argument("duplicate row space path");
      if (space.path == parent) hasParent = true;
    }
    if (finished || !validChildSpacePath(path) || !hasParent) {
      throw std::invalid_argument("invalid row space declaration");
    }
    auto* data = (char*)pool.alloc(path.size());
    std::memcpy(data, path.data(), path.size());
    spaces.push_back({std::string_view(data, path.size()), 0, {}});
    return (SpaceOrdinal)(spaces.size() - 1);
  }

  // Attach the final row count and encoded ownership after indexing.
  void finishSpace(SpaceOrdinal ordinal, int32_t count, OwnershipDescriptor ownership) {
    if (finished || ordinal == SpaceOrdinal::ROOT || !findSpace(ordinal)
        || count <= 0 || !validSpaceDocCount(count) || !ownership.ends) {
      throw std::invalid_argument("invalid row space finalization");
    }
    auto& space = spaces[(size_t)ordinal];
    if (space.ownership.ends) throw std::invalid_argument("row space already finalized");
    space.maxDoc = count;
    space.ownership = ownership;
  }

  const CatalogRange& catalogRange() const noexcept { return catalog; }

  // Charges working memory held until a field finishes, such as an id filter
  // builder, to the owner's budget along with staged streams.
  void accountRam(int64_t delta) {
    if (onStagedRamDelta && delta) onStagedRamDelta(delta);
  }

  int64_t getSizeInBytes() {
    return sizeInBytes;
  }

  // Write liveDocs bitmap for documents that were marked as deleted during indexing
  // Returns the liveGen (generation number) used for the file, or 0 if no file was written
  uint64_t writeLiveDocs(const screaming::FixedBitSet& liveBits, int32_t numLiveDocs);

private:
  const RowSpace* findSpace(SpaceOrdinal ordinal) const {
    return (size_t)ordinal < spaces.size() ? &spaces[(size_t)ordinal] : nullptr;
  }

  void writeCatalog() {
    RAMFile tail("");
    OutputStream out(&tail);
    SegmentCatalogWriter writer(out);
    writeFieldTables(writer);
    std::vector<uint32_t> files;
    files.reserve(spillFiles.size() + 1);
    files.push_back(Postings::PRIMARY_FILE);
    for (const auto& file : spillFiles) files.push_back(file->fileNum);
    writer.finish(files, versions);
    out.close();
    primary->out.align(MAX_ALIGN);
    auto start = primary->out.size();
    checkSliceSize(start, tail.size());
    catalog = {start, tail.size(), tail.digest()};
    primary->out.appendFile(tail);
  }

  void writeFieldTables(SegmentCatalogWriter& writer) {
    std::vector<SpaceOrdinal> order;
    order.reserve(spaces.size());
    for (size_t i = 0; i < spaces.size(); i++) order.push_back((SpaceOrdinal)i);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
      return spaces[(size_t)a].path < spaces[(size_t)b].path;
    });
    // PostingsReader::findOrdinal does a binary search over the field offsets array, so
    // fields must be written in sorted name order.  Callers that flush fields
    // in sorted order already satisfy this, but some resources (like
    // stored-fields) are appended at the very end of flush; sort defensively
    // here to support arbitrary addField ordering.
    std::vector<IndexFieldInfo*> sortedInfos;
    sortedInfos.reserve(fieldInfos.size());
    // A field is present in the catalog only if it has rows in this space.
    for (auto& field : fieldInfos) {
      if (field.header.docsWithField != 0) {
        sortedInfos.push_back(&field);
      } else if (field.header.parts.size() != 0) {
        throw std::logic_error("field has parts but no docs");
      }
    }
    std::sort(sortedInfos.begin(), sortedInfos.end(), [&](const auto* a, const auto* b) {
      return a->space != b->space
          ? spaces[(size_t)a->space].path < spaces[(size_t)b->space].path
          : a->header.name < b->header.name;
    });
    for (size_t i = 1; i < sortedInfos.size(); i++) {
      if (sortedInfos[i - 1]->space == sortedInfos[i]->space
          && sortedInfos[i - 1]->header.name == sortedInfos[i]->header.name) {
        throw std::invalid_argument("duplicate field in row space");
      }
    }
    size_t first = 0;
    for (auto ordinal : order) {
      writer.beginSpace(spaces[(size_t)ordinal]);
      while (first < sortedInfos.size() && sortedInfos[first]->space == ordinal) {
        const auto& field = *sortedInfos[first++];
        writer.addField(field);
      }
    }
  }

};


using OutputStreamPtr = PostingsWriter::OutputStreamPtr;  // for convenience


class TextNormsView {
  std::span<const uint8_t> ordinalNorms;
  const screaming::BitSet* docsBitset = nullptr;
  mutable std::optional<screaming::BitSet::Iterator> docsIter;
  mutable int32_t lastDoc = -1;

public:
  TextNormsView() = default;

  TextNormsView(std::span<const uint8_t> ordinalNorms, const screaming::BitSet* docsBitset)
      : ordinalNorms(ordinalNorms), docsBitset(docsBitset) {
  }

  bool empty() const {
    return ordinalNorms.empty();
  }

  uint32_t normForDoc(int32_t docid) const {
    if (ordinalNorms.empty()) {
      return 0;
    }
    if (docsBitset == nullptr) {
      assert(docid >= 0 && (size_t)docid < ordinalNorms.size());
      return (uint32_t) ordinalNorms[(size_t)docid];
    }

    if (!docsIter.has_value() || docid <= lastDoc) {
      docsIter.emplace(*docsBitset);
    }
    [[maybe_unused]] int32_t found = docsIter->advance(docid);
    assert(found == docid);
    lastDoc = docid;
    int32_t ord = docsIter->rank();
    assert(ord >= 0 && (size_t)ord < ordinalNorms.size());
    return (uint32_t) ordinalNorms[(size_t)ord];
  }
};


// A (norm -> maxTf) surface that tracks which norms are set, so raising,
// walking, and clearing it cost the norms present rather than all 256: most
// terms touch a handful of norms.
struct NormSurface {
  static constexpr uint32_t NUM_NORMS = 256;
  std::array<uint32_t, NUM_NORMS> maxTf{};
  std::array<uint64_t, NUM_NORMS / 64> present{};

  void raise(uint32_t norm, uint32_t tf) {
    assert(norm <= 255);
    present[norm >> 6] |= 1ull << (norm & 63);
    maxTf[norm] = std::max(maxTf[norm], tf);
  }

  // Calls fn(norm, tf) for every point of the Pareto frontier: norms ascending
  // where maxTf strictly rises.
  template <class Fn>
  void forEachFrontierPoint(Fn&& fn) const {
    uint32_t runningMaxTf = 0;
    for (uint32_t word = 0; word < present.size(); word++) {
      for (uint64_t bits = present[word]; bits != 0; bits &= bits - 1) {
        uint32_t norm = word * 64 + (uint32_t) std::countr_zero(bits);
        uint32_t tf = maxTf[norm];
        if (tf > runningMaxTf) {
          fn(norm, tf);
          runningMaxTf = tf;
        }
      }
    }
  }

  void clear() {
    for (uint32_t word = 0; word < present.size(); word++) {
      for (uint64_t bits = present[word]; bits != 0; bits &= bits - 1) {
        maxTf[word * 64 + (uint32_t) std::countr_zero(bits)] = 0;
      }
      present[word] = 0;
    }
  }
};

// Run size settings for the term dictionary writer.
class TextWriterSettings {
protected:
  // Leave enough space in a run for one more block at a 32-bit offset.
  // A block of 32 terms uses at most 8,285 bytes for the packed first term,
  // two vlong bases, prefix length, pulse mask, suffix length vint, hashes,
  // prefix/suffix lengths and 31 full suffixes. Seven run lengths take 35
  // bytes. Docs ends, TTF and positions take 960 bytes (three vlongs per
  // term). Packed-block counts and doc frequencies take 272 bytes (two SVB
  // runs). Pulsed docs and positions take another 272 (64 SVB values).
  // Each impact frontier has a two-byte count and up to 256 points of one
  // norm byte and a five-byte TF delta, or 49,216 bytes for the block.
  // The sum is at most 59,040 bytes, even charging all lanes at once.
  static constexpr uint32_t MAX_TERM_BLOCK_BYTES = 64 * 1024;
  static constexpr uint32_t RUN_SPLIT_BYTES =
      std::numeric_limits<field_parts::Terms::BlockOffset>::max() - MAX_TERM_BLOCK_BYTES;

private:
  static constexpr uint32_t TERMS = Postings::TERMS_BLOCK_SIZE;
  static constexpr uint32_t HEADER_BYTES = PackedTerm::MAX_BYTES + 2 * MAX_VLONG_SIZE
      + 1 + sizeof(uint32_t) + MAX_VINT_SIZE;
  static constexpr uint32_t TERM_BYTES = TERMS + (TERMS - 1) * (2 + PackedTerm::MAX_LEN);
  static constexpr uint32_t MAX_BLOCK_BYTES = HEADER_BYTES + TERM_BYTES + 7 * MAX_VINT_SIZE
      + 3 * TERMS * MAX_VLONG_SIZE + 2 * (svbKeyBytes(TERMS) + TERMS * sizeof(uint32_t))
      + svbKeyBytes(2 * TERMS) + 2 * TERMS * sizeof(uint32_t)
      + TERMS * (2 + NormSurface::NUM_NORMS * (1 + MAX_VINT_SIZE));
  static_assert(NormSurface::NUM_NORMS >= 128 && NormSurface::NUM_NORMS < (1u << 14));
  static_assert(TERMS == std::numeric_limits<uint32_t>::digits);
  static_assert(MAX_BLOCK_BYTES == 59040 && MAX_BLOCK_BYTES < MAX_TERM_BLOCK_BYTES);

protected:
  inline static std::atomic<uint32_t> splitBytes = RUN_SPLIT_BYTES;  // Writer setting, sampled at construction.

public:
  static uint32_t setRunSplitBytesForTests(uint32_t bytes) {
    return splitBytes.exchange(std::min(bytes, RUN_SPLIT_BYTES));
  }
};

// Writes a field or partition of the term dictionary and postings.
// TODO: Specialize the writer for fields without positions.
class TextWriter : public TextWriterSettings {
public:
  static constexpr int32_t L1_PERIOD = 32;
  static constexpr int32_t kCheckpointStride = 8;
  static inline int32_t checkpointStrideForTests = kCheckpointStride;

  // A run's start in the partition's three streams, before file placement.
  struct RunStart {
    int32_t termOrd;  // First term ordinal in the partition.
    int32_t blockOrd;  // First block ordinal in the partition.
    uint64_t dictionaryStart;  // Byte offset relative to the partition's dictionary.
    uint64_t docsStart;  // Byte offset relative to the partition's docs stream.
    uint64_t positionsStart;  // Byte offset relative to the partition's positions stream.
  };

  // A finished serial field or partition before stitching its dictionary.
  struct RangeResult {
    std::vector<field_parts::Terms::BlockOffset> termBlockOffsets;  // Relative to each run.
    std::vector<RunStart> runs;  // Cuts at term block boundaries.
    std::vector<std::string> separatorKeys;
    std::string firstTerm;
    std::string lastTerm;
    int64_t sumDocFreq = 0;
    int64_t sumTotalTermFreq = 0;
    uint64_t dictionaryBytes = 0;
    uint64_t docsBytes = 0;
    uint64_t posBytes = 0;
    int64_t nTerms = 0;
    int32_t nBlocks = 0;
    uint8_t segFlags = 0;  // Data facts collected while writing this partition.

    void appendRuns(std::vector<field_parts::TermRun>& destination, int32_t termOrd, int32_t blockOrd,
                    uint64_t dictionaryStart, field_parts::Slice docs, field_parts::Slice positions) const {
      assert(docs.bytes == docsBytes && positions.bytes == posBytes);
      auto slice = [](field_parts::Slice stream, uint64_t start, uint64_t end) {
        return end == start ? field_parts::Slice{} : stream.subSlice(start, end - start);
      };
      for (size_t i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        const auto* next = i + 1 == runs.size() ? nullptr : &runs[i + 1];
        destination.push_back({termOrd + run.termOrd, blockOrd + run.blockOrd,
            dictionaryStart + run.dictionaryStart,
            slice(docs, run.docsStart, next ? next->docsStart : docsBytes),
            slice(positions, run.positionsStart, next ? next->positionsStart : posBytes)});
      }
    }
  };

  static std::string separatorKey(std::string_view previous, std::string_view first) {
    uint32_t minLen = (uint32_t) std::min(previous.size(), first.size());
    uint32_t mismatch = 0;
    while (mismatch < minLen
           && (uint8_t) previous[mismatch] == (uint8_t) first[mismatch]) {
      mismatch++;
    }
    assert(mismatch < first.size());
    if (mismatch < previous.size()) {
      assert((uint8_t) previous[mismatch] < (uint8_t) first[mismatch]);
    }
    return std::string(first.substr(0, mismatch + 1));
  }

private:
  PostingsWriter& postingsWriter;
  SpaceOrdinal space;
  PackedTerm name = PackedTerm(nullptr);
  FieldType::Type type = FieldType::NONE;
  FieldType::flag_type flags = 0;  // Schema flags for the field being written.

  std::array<OutputStreamPtr, 3> streams; // holders for the streams we are using.
  OutputStream& termOutput;    // output stream for termFile
  OutputStream& docOutput;     // output stream for docFile
  OutputStream& posOutput;     // output stream for posFile


  //
  // variable naming:
  // *loc* refers to absolute locations in a file, usually obtained via OutputStream::size()
  // *off* / *offset* refers to offsets relative to something else (i.e. a location)
  //

  // needed to build each block
  std::vector<TermRef> termList;  // list of terms in the current term block, pointing into termBytes
  // Block-local term storage: startTerm() copies each term here because flushTerms()
  // re-reads the block's terms for prefix encoding, so callers need not keep term
  // bytes alive past the startTerm() call.  Slot i backs termList[i].
  std::array<char, Postings::TERMS_BLOCK_SIZE * PackedTerm::MAX_BYTES> termBytes;
  std::vector<uint64_t> termDocsEnd;  // cumulative docs stream end offsets
  std::vector<uint32_t> termPackedBlocks;
  std::vector<uint32_t> termDocFreqs;
  std::vector<uint64_t> termTtfCodes;
  std::vector<uint64_t> termPosOffsets;
  std::vector<uint32_t> pulsed; // pulsed doc/pos values for terms with a single occurrence
  uint32_t termPulsedMask = 0;

  // needed for each term
  std::vector<int32_t> docs; // list of documents containing a term
  std::vector<int32_t> tfreqs; // term freqs - number of times the term appears in each document (parallel vector to "docs")
  std::vector<int32_t> posdeltas; // list of position deltas for the current term (for all documents... per-document positions are not delimited)

  // base (starting) values int the associated output streams to calculate offsets from
  int64_t termsLoc=0;
  int64_t docsLoc;
  int64_t posLoc;

  int64_t locOfPositionsForTermBlock;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTerm;
  int64_t locOfDocsForTerm;

  // Absolute posOutput location of the position block that holds the current
  // doc block's first position.  Captured when a doc block starts (startDoc
  // with an empty docs buffer): at that moment posOutput holds exactly the
  // fully flushed position blocks, so its size is the in-progress block's
  // start - the block the next position lands in.  Consumed by appendL0Block.
  int64_t pendingBlockPosByteOff = 0;

  int32_t docsFlushed;  /// number of documents flushed for the current term so far
  uint32_t termPackedBlockCount = 0;
  int32_t curTf = 0;    /// occurrences (term freq) seen so far for the current doc
  int64_t ttfAcc = 0;   /// total term freq (sum of tf over docs) accumulated for the current term
  uint32_t prevDocBlockLast = 0;  /// last doc id of the previous flushed doc block (cross-block delta base); reset per term
  uint32_t prevL1GroupLast = 0;   /// last doc id of the previous flushed L1 group; reset per term
  uint32_t l1GroupLastDoc = 0;    /// last doc id in the buffered L1 group
  int32_t l1GroupBlockCount = 0;
  uint32_t l1GroupPackedBlockCount = 0;
  int32_t l1GroupDocCount = 0;
  uint64_t l1GroupTfSum = 0;
  uint32_t l1GroupMaxTf = 0;
  uint32_t l1GroupMinNorm = 0;
  std::array<uint32_t, L1_PERIOD> l1BlockLastDocs{};
  std::array<uint64_t, L1_PERIOD> l1BlockBodyOffsets{};
  // Group-level (norm -> maxTf) surface for the buffered L1 group, merged from
  // each flushed block's frontier points.  The group header stores its Pareto
  // frontier (same staircase as L0 and the term dictionary) so group bounds
  // come from real (norm, tf) pairs, not a cross-doc (maxTf, minNorm) corner.
  NormSurface l1GroupSurface;

  // Index level for this field, decoded from the field flags in startField().  These gate
  // whether the freq stream and position stream are written at all.
  bool hasFreqs = false;
  bool hasPositions = false;
  bool hasNorms = false;
  TextNormsView norms;

  std::vector<field_parts::Terms::BlockOffset> termBlockOffsets;  // Block starts relative to their run.
  std::vector<RunStart> runs;  // Nonempty runs in this field or partition.
  const uint32_t runSplitBytes = splitBytes.load(std::memory_order_relaxed);
  TrieBuilder trieBuilder;
  std::vector<std::string> separatorKeys;
  std::string firstTerm;
  bool buildTrie = true;
  std::array<char, PackedTerm::MAX_BYTES> lastTermOfPrevBlock{};
  uint32_t lastTermOfPrevBlockLen = 0;
  bool hasLastTermOfPrevBlock = false;
  int64_t sumTotalTermFreq = 0; // updated in endTerm
  int64_t sumDocFreq = 0; // updated in endTerm
  int64_t numTerms;  // Number of terms in completed dictionary blocks.
  uint8_t segFlags = 0;  // Data facts from the kept terms.
  // A root ID dictionary's membership filter, fed with the hash each term block keeps.
  std::optional<IdFilter::Builder> idFilter;
  int64_t idFilterRam = 0;  // Charged for idFilter.

  // TODO: pool allocate this
  std::vector<char> compressed_output;
  std::vector<char> header_output;
  std::vector<char> group_output;
  NormSurface blockSurface;
  std::vector<uint32_t> frontierNorms;
  std::vector<uint32_t> frontierTfs;
  // Term-level (maxTf, norm) surface accumulated across the whole term, so the
  // term dictionary can store the term's impact frontier: a scorer's global
  // max score must come from stored data, never a walk of the postings.
  NormSurface termSurface;
  std::vector<char> termImpactRun;  // per-term encoded frontiers for the current term block
  static inline bool disableL0CheckpointWrite =
      std::getenv("LUXIR_DISABLE_L0_CHECKPOINT_WRITE") != nullptr;

private:  // some internal utility methods... not for use by indexers
  void anchorPositionedDocBlock() {
    if (hasPositions && docs.empty()) {
      pendingBlockPosByteOff = posOutput.size();
    }
  }

  void appendPositionDeltasRaw(std::span<const int32_t> deltas) {
    while (!deltas.empty()) {
      size_t count = std::min(
          deltas.size(), Postings::POSITIONS_BLOCK_SIZE - posdeltas.size());
      posdeltas.insert(posdeltas.end(), deltas.begin(), deltas.begin() + count);
      deltas = deltas.subspan(count);
      if (posdeltas.size() == Postings::POSITIONS_BLOCK_SIZE) {
        flushPositions();
      }
    }
  }

  static void appendVint(std::vector<char>& out, uint32_t val) {
    while (val > 0x7f) {
      out.push_back((char) (val | 0x80));
      val >>= 7;
    }
    out.push_back((char) val);
  }

  static void appendVlong(std::vector<char>& out, uint64_t val) {
    while (val > 0x7f) {
      out.push_back((char) (val | 0x80));
      val >>= 7;
    }
    out.push_back((char) val);
  }

  static void appendShortLE(std::vector<char>& out, uint16_t val) {
    out.push_back((char) val);
    out.push_back((char) (val >> 8));
  }

  static void appendIntLE(std::vector<char>& out, uint32_t val) {
    out.push_back((char) val);
    out.push_back((char) (val >> 8));
    out.push_back((char) (val >> 16));
    out.push_back((char) (val >> 24));
  }

  static void appendVint15(std::vector<char>& out, uint32_t val) {
    if ((val & ~0x7fffu) == 0) {
      appendShortLE(out, (uint16_t) val);
    } else {
      appendShortLE(out, (uint16_t) (0x8000u | (val & 0x7fffu)));
      appendVint(out, val >> 15);
    }
  }

  static void appendVlong15(std::vector<char>& out, uint64_t val) {
    if ((val & ~0x7fffull) == 0) {
      appendShortLE(out, (uint16_t) val);
    } else {
      appendShortLE(out, (uint16_t) (0x8000u | (val & 0x7fffull)));
      appendVlong(out, val >> 15);
    }
  }

  static uint32_t commonPrefixLen(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    uint32_t len = std::min(alen, blen);
    uint32_t prefixLen = 0;
    while (prefixLen < len && a[prefixLen] == b[prefixLen]) {
      prefixLen++;
    }
    return prefixLen;
  }

  uint32_t normForDoc(int32_t docid) const {
    return norms.normForDoc(docid);
  }

  uint32_t buildImpactFrontier(uint32_t docCount) {
    frontierNorms.resize(0);
    frontierTfs.resize(0);
    if (!hasNorms) {
      return 0;
    }

    uint32_t minNorm = 255;
    if (!hasFreqs) {
      for (uint32_t i = 0; i < docCount; i++) {
        minNorm = std::min(minNorm, normForDoc(docs[i]));
      }
      return minNorm;
    }

    blockSurface.clear();
    for (uint32_t i = 0; i < docCount; i++) {
      uint32_t norm = normForDoc(docs[i]);
      assert(norm <= 255);
      uint32_t tf = (uint32_t) tfreqs[i];
      minNorm = std::min(minNorm, norm);
      blockSurface.raise(norm, tf);
      termSurface.raise(norm, tf);
    }

    blockSurface.forEachFrontierPoint([&](uint32_t norm, uint32_t tf) {
      frontierNorms.push_back(norm);
      frontierTfs.push_back(tf);
    });
    return minNorm;
  }

  // Encode the Pareto frontier of a (norm -> maxTf) surface as
  // [count vint][(norm u8, tf-delta vint)*], the same staircase the L0
  // headers use.  count == 0 only for fields without freqs+norms.
  static void appendImpactFrontier(std::vector<char>& out, const NormSurface& surface) {
    uint32_t count = 0;
    surface.forEachFrontierPoint([&](uint32_t, uint32_t) { count++; });
    appendVint(out, count);
    uint32_t runningMaxTf = 0;
    surface.forEachFrontierPoint([&](uint32_t norm, uint32_t tf) {
      out.push_back((char) (uint8_t) norm);
      appendVint(out, tf - runningMaxTf);
      runningMaxTf = tf;
    });
  }

  static void appendL1ImpactFrontier(std::vector<char>& out, const NormSurface& surface,
                                     uint32_t maxTf) {
    uint32_t count = 0;
    surface.forEachFrontierPoint([&](uint32_t, uint32_t) { count++; });
    assert(count > 0);
    bool useU32 = maxTf > 65535u;
    appendVint(out, (count << 1) | (useU32 ? 1u : 0u));
    surface.forEachFrontierPoint([&](uint32_t norm, uint32_t) {
      out.push_back((char) (uint8_t) norm);
    });
    surface.forEachFrontierPoint([&](uint32_t, uint32_t tf) {
      if (useU32) {
        appendIntLE(out, tf);
      } else {
        assert(tf <= 65535u);
        appendShortLE(out, (uint16_t) tf);
      }
    });
  }

  void appendL0Header(std::vector<char>& out, uint32_t lastDoc, uint32_t base,
                      uint64_t blockByteLen, uint32_t docCount, uint64_t tfSum,
                      uint32_t maxTf, uint32_t minNorm, uint64_t posByteOff) {
    header_output.resize(0);
    assert(lastDoc >= base);
    appendVint15(header_output, lastDoc - base);
    appendVlong15(header_output, blockByteLen);
    if (hasPositions) {
      assert(tfSum >= docCount);
      appendVint(header_output, (uint32_t) (tfSum - docCount));
      // Byte offset (from the term's position start) of the position block
      // holding this doc block's first position.  The ord within that block is
      // not stored: it is cumTf-before-block % POSITIONS_BLOCK_SIZE, which the
      // reader already tracks via the tfSum chain.
      appendVlong(header_output, posByteOff);
    }
    if (hasFreqs && hasNorms) {
      assert(!frontierTfs.empty());
      assert(frontierTfs.size() == frontierNorms.size());
      appendVint(header_output, (uint32_t) frontierTfs.size());
      uint32_t prevTf = 0;
      for (size_t i = 0; i < frontierTfs.size(); i++) {
        uint32_t norm = frontierNorms[i];
        uint32_t tf = frontierTfs[i];
        assert(norm <= 255);
        assert(i == 0 || norm > frontierNorms[i - 1]);
        assert(tf > prevTf);
        // norm is a SmallFloat byte (0-255): store it raw, not vint-encoded.  tf is
        // unbounded, so keep its delta (the staircase is tf-ascending) vint-encoded.
        header_output.push_back((char) (uint8_t) norm);
        appendVint(header_output, tf - prevTf);
        prevTf = tf;
      }
    } else if (hasFreqs) {
      assert(maxTf > 0);
      appendVint(header_output, maxTf);
    } else if (hasNorms) {
      assert(minNorm <= 255);
      appendVint(header_output, minNorm);
    }
    appendVint(out, (uint32_t) header_output.size());
    appendBytes(out, header_output.data(), header_output.size());
  }

  void appendBytes(std::vector<char>& out, const void* data, size_t len) {
    const char* src = (const char*) data;
    out.insert(out.end(), src, src + len);
  }

  void appendVlongRun(std::vector<char>& out, const std::vector<uint64_t>& values) {
    for (uint64_t val : values) {
      appendVlong(out, val);
    }
  }

  void appendVlongDeltaRun(std::vector<char>& out, const std::vector<uint64_t>& values) {
    uint64_t prev = 0;
    for (uint64_t val : values) {
      assert(val >= prev);
      appendVlong(out, val - prev);
      prev = val;
    }
  }

  void appendSVBRun(std::vector<char>& out, const std::vector<uint32_t>& values) {
    if (values.empty()) {
      return;
    }
    uint32_t count = (uint32_t) values.size();
    uint32_t keyBytes = svbKeyBytes(count);
    std::vector<uint8_t> keys(keyBytes);
    std::vector<uint8_t> data((size_t) count * sizeof(uint32_t));
    uint8_t* dataEnd = svb_encode_scalar(values.data(), keys.data(), data.data(), count);
    appendBytes(out, keys.data(), keys.size());
    appendBytes(out, data.data(), (size_t) (dataEnd - data.data()));
  }

  void writeMetadataRunLen(size_t byteLen) {
    assert(byteLen <= (size_t) (UINT32_MAX >> 1));
    termOutput.writeVint(((uint32_t) byteLen) << 1u);
  }

  static uint32_t termMaskForCount(int nTerms) {
    assert(nTerms > 0 && nTerms <= Postings::TERMS_BLOCK_SIZE);
    return nTerms == 32 ? UINT32_MAX : ((1u << (uint32_t) nTerms) - 1u);
  }

  static bool allUInt64Equal(const std::vector<uint64_t>& values, uint64_t target) {
    for (uint64_t value : values) {
      if (value != target) {
        return false;
      }
    }
    return true;
  }

  static bool allUInt32Equal(const std::vector<uint32_t>& values, uint32_t target) {
    for (uint32_t value : values) {
      if (value != target) {
        return false;
      }
    }
    return true;
  }

  void addTrieSeparatorForCurrentBlock(uint32_t blockOrd) {
    if (blockOrd == 0) {
      return;
    }

    // Block 0 uses the trie's implicit empty separator.  For later blocks, the
    // separator is the first term truncated one byte past its first mismatch
    // with the previous block's last term.  That key is strictly greater than
    // every term in the previous block and <= the first term in this block, so
    // floorBlock routes between-block gaps to the later block.
    auto [firstData, firstLen] = termList[0].unpack();
    assert(hasLastTermOfPrevBlock);
    std::string key = separatorKey(
        std::string_view(lastTermOfPrevBlock.data(), lastTermOfPrevBlockLen),
        std::string_view(firstData, firstLen));
    if (buildTrie) {
      trieBuilder.add(key, blockOrd);
    } else {
      separatorKeys.push_back(std::move(key));
    }
  }

  void rememberLastTermOfCurrentBlock() {
    auto [lastData, lastLen] = termList.back().unpack();
    assert(lastLen <= PackedTerm::MAX_LEN);
    memcpy(lastTermOfPrevBlock.data(), lastData, lastLen);
    lastTermOfPrevBlockLen = lastLen;
    hasLastTermOfPrevBlock = true;
  }

  void appendL0Block(uint32_t lastDoc, uint32_t base, uint64_t blockByteLen,
                     uint32_t docCount, uint64_t tfSum, uint32_t maxTf, uint32_t minNorm) {
    assert(l1GroupBlockCount >= 0 && l1GroupBlockCount < L1_PERIOD);
    l1BlockLastDocs[(size_t) l1GroupBlockCount] = lastDoc;
    l1BlockBodyOffsets[(size_t) l1GroupBlockCount] =
        (uint64_t) group_output.size();
    // Position anchor for this doc block, captured at its first startDoc.
    assert(pendingBlockPosByteOff >= locOfPositionsForTerm);
    uint64_t posByteOff = hasPositions
        ? (uint64_t) (pendingBlockPosByteOff - locOfPositionsForTerm) : 0;
    appendL0Header(group_output, lastDoc, base, blockByteLen, docCount, tfSum, maxTf, minNorm,
                   posByteOff);
    appendBytes(group_output, compressed_output.data(), blockByteLen);
    l1GroupLastDoc = lastDoc;
    l1GroupBlockCount++;
    l1GroupDocCount += (int32_t) docCount;
    if (hasPositions) {
      l1GroupTfSum += tfSum;
    }
    if (hasFreqs) {
      l1GroupMaxTf = std::max(l1GroupMaxTf, maxTf);
    }
    if (hasNorms) {
      assert(minNorm <= 255);
      l1GroupMinNorm = l1GroupBlockCount == 1 ? minNorm : std::min(l1GroupMinNorm, minNorm);
    }
    if (hasFreqs && hasNorms) {
      // Merge this block's frontier (still live from buildImpactFrontier) into
      // the group surface; dominated points fall out when the group frontier is
      // re-extracted at flush.
      assert(frontierTfs.size() == frontierNorms.size());
      for (size_t i = 0; i < frontierNorms.size(); i++) {
        l1GroupSurface.raise(frontierNorms[i], frontierTfs[i]);
      }
    }
    if (l1GroupBlockCount == L1_PERIOD) {
      flushL1Group();
    }
  }

  void flushL1Group() {
    if (l1GroupBlockCount == 0) {
      return;
    }
    header_output.resize(0);
    assert(l1GroupLastDoc >= prevL1GroupLast);
    appendVint15(header_output, l1GroupLastDoc - prevL1GroupLast);
    appendVlong15(header_output, group_output.size());
    assert(l1GroupPackedBlockCount <= (uint32_t) L1_PERIOD);
    header_output.push_back((char) l1GroupPackedBlockCount);
    assert(checkpointStrideForTests > 0);
    uint8_t entryCount = disableL0CheckpointWrite
        ? 0
        : (uint8_t) ((l1GroupBlockCount - 1) / checkpointStrideForTests);
    header_output.push_back((char) entryCount);
    if (!disableL0CheckpointWrite) {
      [[maybe_unused]] uint32_t previousKey = 0;
      for (int32_t b = checkpointStrideForTests; b < l1GroupBlockCount;
           b += checkpointStrideForTests) {
        uint32_t key = l1BlockLastDocs[(size_t) b - 1];
        uint64_t bodyOffset = l1BlockBodyOffsets[(size_t) b];
        assert(b == checkpointStrideForTests || key > previousKey);
        assert(bodyOffset < (1u << 24));
        assert(b > 0 && b < L1_PERIOD);
        uint64_t entry = ((uint64_t) key << 32)
            | ((uint64_t) (uint32_t) b << 24) | bodyOffset;
        static_assert(std::endian::native == std::endian::little);
        size_t offset = header_output.size();
        header_output.resize(offset + sizeof(entry));
        memcpy(header_output.data() + offset, &entry, sizeof(entry));
        previousKey = key;
      }
    }
    if (hasPositions) {
      assert(l1GroupTfSum >= (uint64_t) l1GroupDocCount);
      appendVint(header_output, (uint32_t) (l1GroupTfSum - (uint64_t) l1GroupDocCount));
    }
    if (hasFreqs && hasNorms) {
      assert(l1GroupMaxTf > 0);
      appendL1ImpactFrontier(header_output, l1GroupSurface, l1GroupMaxTf);
    } else if (hasFreqs) {
      assert(l1GroupMaxTf > 0);
      appendVint(header_output, l1GroupMaxTf);
    } else if (hasNorms) {
      assert(l1GroupMinNorm <= 255);
      appendVint(header_output, l1GroupMinNorm);
    }
    docOutput.writeVint((uint32_t) header_output.size());
    docOutput.write(header_output.data(), header_output.size());
    docOutput.write(group_output.data(), group_output.size());

    prevL1GroupLast = l1GroupLastDoc;
    group_output.resize(0);
    l1GroupBlockCount = 0;
    l1GroupPackedBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
    l1GroupMinNorm = 0;
    l1BlockLastDocs.fill(0);
    l1BlockBodyOffsets.fill(0);
    l1GroupSurface.clear();
  }

  // number of docs for the current term
  int32_t getDocFreq() const {
    return docsFlushed + docs.size();
  }

  // total term freq (sum of tf over all docs) for the current term.  Equals the
  // total position count for fields that index positions.
  int64_t getTotalTermFreq() const {
    return ttfAcc;
  }

  // the size the docfile takes
  int32_t getDocFileSize() const {
    auto sz = docOutput.size() - locOfDocsForTerm;
    assert(sz <= UINT_MAX);
    return sz;
  }

public:
  TextWriter(PostingsWriter& postingsWriter)
  : postingsWriter(postingsWriter), streams(postingsWriter.getOutputStreams<3>()), termOutput(*streams[0]), docOutput(*streams[1]), posOutput(*streams[2])
  {
  }

  TextWriter(PostingsWriter& postingsWriter, OutputStream& termOutput,
             OutputStream& docOutput, OutputStream& posOutput)
  : postingsWriter(postingsWriter), termOutput(termOutput), docOutput(docOutput),
    posOutput(posOutput), buildTrie(false) {
  }

  TextWriter(const TextWriter&) = delete;
  ~TextWriter() { dropIdFilter(); }

  void startField(SpaceOrdinal rowSpace, const PostingsWriter::IndexFieldInfo& field) {
    startField(rowSpace, field.header.name, field.header.flags, field.header.type);
  }

  void startField(SpaceOrdinal rowSpace, PackedTerm fieldName, int32_t flags, FieldType::Type fieldType = FieldType::TEXT) {
    if (!name.isNull()) throw std::runtime_error("startField called twice!");
    assert(!fieldName.isNull());
    space = rowSpace;
    name = fieldName;
    type = fieldType;
    this->flags = flags;
    hasFreqs = FieldType::hasFreqs(flags);
    hasPositions = FieldType::hasPositions(flags);
    hasNorms = FieldType::hasNorms(flags);
    termsLoc = termOutput.size();
    docsLoc = docOutput.size();
    posLoc = posOutput.size();
    numTerms = 0;
    segFlags = 0;
    dropIdFilter();
    if (fieldType == FieldType::ID && rowSpace == SpaceOrdinal::ROOT) {
      // Ranges of a partitioned merge would each see part of the keys.
      if (!buildTrie) throw std::logic_error("a root id dictionary must be written by one writer");
      // A segment holds at most one id term per root document.
      if (postingsWriter.idFilterOptions.kind != IdFilter::Kind::NONE) {
        idFilter.emplace(postingsWriter.idFilterOptions, (uint64_t)std::max(postingsWriter.getMaxDoc(SpaceOrdinal::ROOT), 1));
        idFilterRam = (int64_t)idFilter->sizeInBytes();
        postingsWriter.accountRam(idFilterRam);
      }
    }

    termBlockOffsets.resize(0);
    trieBuilder.reset();
    separatorKeys.clear();
    firstTerm.clear();
    lastTermOfPrevBlockLen = 0;
    hasLastTermOfPrevBlock = false;

    startTermBlock();
  }

  // Currently only called for a full block of positions.
  // TODO: move to .cpp unless we template this class... allows for removal of the associated include files
  void flushPositions() {
    if (posdeltas.empty()) {
      return;
    }
    compressed_output.resize(Postings::POSITIONS_BLOCK_SIZE * sizeof(int32_t) + 1024);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    IndexCodec::posCodec.encodeBlock(reinterpret_cast<uint32_t *>(posdeltas.data()), posdeltas.size(), compressed_output.data(),
                                  compressedSize);
    posOutput.write(compressed_output.data(), compressedSize);

    posdeltas.resize(0);
  }

  // TODO: move to .cpp unless we template this class
  void flushDocs() {
    assert(docs.size() == tfreqs.size());

    if (docs.empty()) {
      return;
    }

    assert(docs.back() < postingsWriter.getMaxDoc(space)); // sanity check to ensure we didn't go over provided maxDoc
    assert(docs.size() == Postings::DOCS_BLOCK_SIZE);

    // NOTE: LuxirPFORd (docCodec) applies the adjacent delta itself, in place,
    // so we pass the raw (monotonic) doc ids. encodeBlock handles exactly one
    // DOCS_BLOCK_SIZE block.  The block's first id is coded as a delta from the
    // previous block's last id (cross-block base); capture this block's last id
    // first, since encodeBlock deltas `docs` in place.
    const uint32_t base = prevDocBlockLast;
    const uint32_t lastDoc = (uint32_t) docs.back();
    uint64_t tfSum = 0;
    uint32_t maxTf = 0;
    if (hasFreqs) {
      for (auto tf : tfreqs) {
        if (hasPositions) {
          tfSum += (uint32_t) tf;
        }
        maxTf = std::max(maxTf, (uint32_t) tf);
      }
    }
    uint32_t minNorm = buildImpactFrontier(Postings::DOCS_BLOCK_SIZE);

    // Doc-part encoding decision (token byte + body; see Postings::DOC_BLOCK_*
    // and the postings reader). Dense blocks store doc ids as a bitset over (base, lastDoc]
    // instead of PFor deltas, and a fully consecutive block stores nothing at
    // all. Storage rule as in Lucene: the bitset wins whenever it costs no more
    // than the next-larger packed width, biasing ties toward the bitset (its
    // word-OR count fill and cheaper decode are worth it).
    // docBase = the doc id bit 0 of the bitset form maps to. The term's first
    // block has no previous doc and docid 0 is legal, so its span starts AT the
    // (zero) base; later blocks start one past the previous block's last id.
    // A flushed block's last doc is >= DOCS_BLOCK_SIZE - 1 > 0, so base == 0
    // identifies the first block (mirrors the reader's docOrd == 0 test).
    const uint32_t docBase = base + (base == 0 ? 0 : 1);
    const uint32_t spanBits = lastDoc - docBase + 1;
    assert(spanBits >= (uint32_t) Postings::DOCS_BLOCK_SIZE);
    uint32_t deltaOr = (uint32_t) docs[0] - base;
    for (size_t i = 1; i < docs.size(); i++) {
      deltaOr |= (uint32_t) (docs[i] - docs[i - 1]);
    }
    const uint32_t bitsPerValue = 32 - (uint32_t) std::countl_zero(deltaOr | 1);
    const uint32_t numWords = (spanBits + 63) / 64;

    compressed_output.resize(2 * (Postings::DOCS_BLOCK_SIZE * sizeof(int32_t) + 1024));
    uint32_t blockByteLen;
    if (spanBits == (uint32_t) Postings::DOCS_BLOCK_SIZE) {
      compressed_output[0] = Postings::DOC_BLOCK_CONTIGUOUS;
      blockByteLen = 1;
    } else if (std::min(32u, bitsPerValue + 1) * (uint32_t) Postings::DOCS_BLOCK_SIZE <= numWords * 64) {
      compressed_output[0] = Postings::DOC_BLOCK_PACKED;
      termPackedBlockCount++;
      l1GroupPackedBlockCount++;
      uint32_t compressedSize = (uint32_t) compressed_output.size() - 1; // this gets changed to the actual size
      IndexCodec::docCodec.encodeBlock(reinterpret_cast<uint32_t *>(docs.data()), docs.size(), compressed_output.data() + 1,
                                    compressedSize, base);
      blockByteLen = 1 + compressedSize;
    } else {
      assert(numWords <= 63);
      compressed_output[0] = (char) (int8_t) -(int32_t) numWords;
      uint64_t words[64];
      memset(words, 0, numWords * 8);
      for (auto d : docs) {
        const uint32_t s = (uint32_t) d - docBase;
        words[s >> 6] |= 1ULL << (s & 63);
      }
      memcpy(compressed_output.data() + 1, words, numWords * 8);
      blockByteLen = 1 + numWords * 8;
    }

    //
    // now the term freqs (omitted entirely for DOCS-only fields)
    //
    if (hasFreqs) {
      uint32_t compressedSize = (uint32_t) compressed_output.size() - blockByteLen; // this gets changed to the actual size
      IndexCodec::tfreqCodec.encodeBlock(reinterpret_cast<uint32_t *>(tfreqs.data()), tfreqs.size(), compressed_output.data() + blockByteLen,
                                      compressedSize);
      blockByteLen += compressedSize;
    }

    appendL0Block(lastDoc, base, blockByteLen, Postings::DOCS_BLOCK_SIZE, tfSum, maxTf, minNorm);
    prevDocBlockLast = lastDoc;

    docsFlushed += docs.size();
    docs.resize(0);
    tfreqs.resize(0);
  }


  // Have a different set of methods depending on the field type?
  // For example things could be much simpler if not indexing positions.


  // called starting a new field, or after flushing a term block.
  void startTermBlock() {
    termList.resize(0);
    termDocsEnd.resize(0);
    termPackedBlocks.resize(0);
    termDocFreqs.resize(0);
    termTtfCodes.resize(0);
    termPosOffsets.resize(0);
    termImpactRun.resize(0);
    pulsed.resize(0);
    termPulsedMask = 0;
    locOfPositionsForTermBlock = posOutput.size();
    locOfDocsForTermBlock = docOutput.size();
  }

  // Term block on-disk format.  This writer is the authority; see
  // TermsEnum::readTermBlock/readNextTermInBlock for the matching reader.
  //
  // A field's terms stream contains a sequence of fixed-size term blocks
  // (except the last block).  Each block is laid out as:
  //
  //   header:
  //     [firstTerm: PackedTerm]
  //       The first term is stored whole.  Terms 1..n-1 are front-coded below.
  //     [docsLocDelta: vlong]
  //       Offset of this block's docs region from the run's docs start.
  //     [posLocDelta: vlong]
  //       Offset of this block's positions region from the run's positions start.
  //     [blockPrefixLen: u8]
  //       lcp(first term, last term).  These bytes are factored out of every
  //       in-block suffix and restored from the previous materialized term.
  //     [pulsedMask: u32 little-endian]
  //       Bit i is set when term i is pulsed, meaning its single doc, and
  //       optional position, are stored in the pulsed metadata run instead of
  //       occupying bytes in the docs stream.
  //     [suffixBytesTotal: vint]
  //       Total bytes in the dense suffix blob.  This lets the reader find the
  //       metadata section on block entry without summing suffix lengths.
  //
  //   term scan section:
  //     [hashes: n x u8]
  //       One low byte of XXH3 per term, used to reject most seekExact misses
  //       before materializing term bytes.
  //     [prefixLen: (n-1) x raw u8]
  //     [suffixLen: (n-1) x raw u8]
  //       For term i > 0, prefixLen is measured against the previous term
  //       after removing blockPrefixLen from both terms.  suffixLen is the
  //       remaining byte count.  The first term is the header firstTerm.
  //     [suffix blob: suffixBytesTotal bytes]
  //       Dense concatenation of suffix bytes for terms 1..n-1.
  //
  //   metadata section:
  //     Each sub-run begins with a vint code: (byteLen << 1) | encodingFlag.
  //     The normal v1 encoding writes encodingFlag 0 and a nonzero byteLen.
  //     Code 0 (byteLen 0, flag 0) is claimed as an all-default run marker;
  //     otherwise the low flag bit remains reserved for a future alternate
  //     encoding.  The byte-length vints are written first for every run,
  //     followed by the non-default run bytes in the same order:
  //
  //     [docsEnd lengths/code][packedBlocks lengths/code][df lengths/code]
  //     [ttfCode lengths/code?]
  //     [posOff lengths/code?][termImpact lengths/code?][pulsed lengths/code]
  //     [docsEnd: cumulative vlong run]
  //       Cumulative slice ends within this block's docs region. For term i,
  //       docsSize_i = docsEnd_i - docsEnd_(i-1), with docsEnd_(-1) = 0.
  //       Pulsed terms repeat the previous end, so their docsSize is 0.
  //       Default: every slice is zero-size, which is legal only when every
  //       term in the block is pulsed.
  //     [packedBlocks: StreamVByte run]
  //       Number of full doc blocks encoded with DOC_BLOCK_PACKED for every
  //       term in the block.  Pulsed terms and StreamVByte tail blocks are 0.
  //       Default: every value is 0.
  //     [df: StreamVByte run]
  //       docFreq for every term in the block.
  //       Default: every value is 1.
  //     [ttfCode: vlong run, only when the field indexes freqs]
  //       totalTermFreq - docFreq for every term.
  //       Default: every value is 0.
  //     [posOff: delta vlong run, only when the field indexes positions]
  //       Position-stream start offsets within this block's positions region.
  //       Pulsed terms repeat the running value; readers ignore the offset for
  //       pulsed terms.
  //       Default: every delta is 0, so every offset is 0.
  //     [termImpact: frontier run, only when the field indexes freqs+norms]
  //       Per term, the whole-term (norm, maxTf) Pareto frontier as
  //       [count vint][(norm u8, tf-delta vint)*] - the stored source for a
  //       scorer's global max score.  Never default-coded.
  //     [pulsed: StreamVByte run]
  //       Values for pulsed terms only, in term order.  Each pulsed term stores
  //       doc, and also pos when positions are indexed.  The value index for
  //       term i is popcount(pulsedMask below bit i), scaled by the per-term
  //       value count.
  //
  // The layout is column-stride on purpose.  Term scans, seekCeil, fuzzy
  // enumeration, and most miss paths need only hashes, lengths, and suffix
  // bytes.  Term stats decode as tier 1 (df/ttfCode), and postings-open
  // metadata decodes as tier 2 (docsEnd/packedBlocks/posOff/pulsed), both
  // lazily in TermsEnum.  Per-doc tf values are not stored here: they are
  // scoring data block-encoded alongside doc ids in the docs stream and
  // remain part of the postings payload.
  void flushTerms(bool endingField) {
    if (termList.empty()) return;
    if (!Postings::fitsTermBlocks(termBlockOffsets.size(), 1)) {
      throw std::length_error(Postings::TERM_BLOCK_LIMIT_ERROR);
    }

    auto blockOrd = (uint32_t)termBlockOffsets.size();
    auto blockStart = termOutput.size();
    auto dictionaryStart = blockStart - termsLoc;
    // The block is nonempty. Its postings bases were captured before its first term.
    if (runs.empty() || dictionaryStart - runs.back().dictionaryStart > runSplitBytes) {
      runs.push_back({(int32_t)numTerms, (int32_t)blockOrd, dictionaryStart,
          (uint64_t)(locOfDocsForTermBlock - docsLoc), (uint64_t)(locOfPositionsForTermBlock - posLoc)});
    }
    const auto& run = runs.back();
    termBlockOffsets.push_back((field_parts::Terms::BlockOffset)(dictionaryStart - run.dictionaryStart));
    addTrieSeparatorForCurrentBlock(blockOrd);

    numTerms += termList.size();

    int nTerms = termList.size();
    assert((int)termDocsEnd.size() == nTerms);
    assert((int)termPackedBlocks.size() == nTerms);
    assert((int)termDocFreqs.size() == nTerms);
    assert((int)termTtfCodes.size() == nTerms);
    assert(!hasPositions || (int)termPosOffsets.size() == nTerms);

    TermRef reference = termList[0];
    auto [firstData, firstLen] = reference.unpack();
    auto [lastData, lastLen] = termList.back().unpack();
    // Stitch uses the partition's first term to build its separator.
    if (blockOrd == 0) {
      firstTerm.assign(firstData, firstLen);
    }
    uint32_t blockPrefixLen = commonPrefixLen(firstData, firstLen, lastData, lastLen);
    assert(blockPrefixLen <= UINT8_MAX);

    std::vector<char> prefixLens;
    std::vector<char> suffixLens;
    std::vector<char> suffixBlob;
    prefixLens.reserve((size_t) std::max(0, nTerms - 1));
    suffixLens.reserve((size_t) std::max(0, nTerms - 1));
    suffixBlob.reserve((size_t) nTerms * 8);

    auto [refdata, reflen] = reference.unpack();
    for (int i=1; i<nTerms; i++) {
      auto term = termList[i];
      auto [tdata, tlen] = term.unpack();
      assert(blockPrefixLen <= reflen);
      assert(blockPrefixLen <= tlen);
      assert(memcmp(firstData, tdata, blockPrefixLen) == 0);

      const char* refSuffix = refdata + blockPrefixLen;
      const char* termSuffix = tdata + blockPrefixLen;
      uint32_t refSuffixLen = reflen - blockPrefixLen;
      uint32_t termSuffixLen = tlen - blockPrefixLen;
      uint32_t prefixLen = commonPrefixLen(refSuffix, refSuffixLen, termSuffix, termSuffixLen);
      uint32_t suffixLen = termSuffixLen - prefixLen;
      assert(suffixLen > 0);
      assert(prefixLen <= UINT8_MAX);
      assert(suffixLen <= UINT8_MAX);
      assert(prefixLen == refSuffixLen || refSuffix[prefixLen] != termSuffix[prefixLen]);

      prefixLens.push_back((char) prefixLen);
      suffixLens.push_back((char) suffixLen);
      appendBytes(suffixBlob, termSuffix + prefixLen, suffixLen);

      refdata = tdata;
      reflen = tlen;
    }

    // Header: first term, postings block offsets, block-prefix length,
    // pulsed mask, and total suffix blob bytes.  The total lets the reader
    // locate metadata lazily without summing suffixLens on miss-only visits.
    termOutput.writePackedTerm(reference);
    termOutput.writeVlong(locOfDocsForTermBlock - docsLoc - run.docsStart);
    termOutput.writeVlong(locOfPositionsForTermBlock - posLoc - run.positionsStart);
    termOutput.write((char) blockPrefixLen);
    termOutput.write(&termPulsedMask, sizeof(termPulsedMask));
    termOutput.writeVint((uint32_t) suffixBlob.size());

    // now write hashes of the terms; an id filter takes the whole hash
    for (int i=0; i<nTerms; i++) {
      auto term = termList[i];
      auto hash = IdFilter::hash({term.data(), term.size()});
      termOutput.write((char)hash);
      if (idFilter) idFilter->add(hash);
    }

    termOutput.write(prefixLens.data(), prefixLens.size());
    termOutput.write(suffixLens.data(), suffixLens.size());
    termOutput.write(suffixBlob.data(), suffixBlob.size());

    std::vector<char> docsEndRun;
    std::vector<char> packedBlocksRun;
    std::vector<char> dfRun;
    std::vector<char> ttfRun;
    std::vector<char> posOffRun;
    std::vector<char> pulsedRun;
    appendVlongRun(docsEndRun, termDocsEnd);
    appendSVBRun(packedBlocksRun, termPackedBlocks);
    appendSVBRun(dfRun, termDocFreqs);
    if (hasFreqs) {
      appendVlongRun(ttfRun, termTtfCodes);
    }
    if (hasPositions) {
      appendVlongDeltaRun(posOffRun, termPosOffsets);
    }
    appendSVBRun(pulsedRun, pulsed);
    bool hasTermImpacts = hasFreqs && hasNorms;

    bool docsEndDefault = allUInt64Equal(termDocsEnd, 0);
    if (docsEndDefault) {
      assert(termPulsedMask == termMaskForCount(nTerms));
    }
    bool packedBlocksDefault = allUInt32Equal(termPackedBlocks, 0);
    bool dfDefault = allUInt32Equal(termDocFreqs, 1);
    bool ttfDefault = allUInt64Equal(termTtfCodes, 0);
    bool posOffDefault = hasPositions && allUInt64Equal(termPosOffsets, 0);

    writeMetadataRunLen(docsEndDefault ? 0 : docsEndRun.size());
    writeMetadataRunLen(packedBlocksDefault ? 0 : packedBlocksRun.size());
    writeMetadataRunLen(dfDefault ? 0 : dfRun.size());
    if (hasFreqs) {
      writeMetadataRunLen(ttfDefault ? 0 : ttfRun.size());
    }
    if (hasPositions) {
      writeMetadataRunLen(posOffDefault ? 0 : posOffRun.size());
    }
    if (hasTermImpacts) {
      writeMetadataRunLen(termImpactRun.size());
    }
    writeMetadataRunLen(pulsedRun.size());

    if (!docsEndDefault) {
      termOutput.write(docsEndRun.data(), docsEndRun.size());
    }
    if (!packedBlocksDefault) {
      termOutput.write(packedBlocksRun.data(), packedBlocksRun.size());
    }
    if (!dfDefault) {
      termOutput.write(dfRun.data(), dfRun.size());
    }
    if (hasFreqs && !ttfDefault) {
      termOutput.write(ttfRun.data(), ttfRun.size());
    }
    if (hasPositions && !posOffDefault) {
      termOutput.write(posOffRun.data(), posOffRun.size());
    }
    if (hasTermImpacts) {
      termOutput.write(termImpactRun.data(), termImpactRun.size());
    }
    termOutput.write(pulsedRun.data(), pulsedRun.size());
    if (termOutput.size() - blockStart > MAX_TERM_BLOCK_BYTES) {
      throw std::length_error("encoded term block exceeds the run split allowance");
    }

    [[maybe_unused]] uint32_t pulsedTerms = std::popcount(termPulsedMask);
    assert(pulsed.size() == (size_t)pulsedTerms * (hasPositions ? 2u : 1u));
    rememberLastTermOfCurrentBlock();

    if (!endingField) {
      startTermBlock();
    }
  }


  /// The term is copied into block-local storage and only needs to be valid for the
  /// duration of this call.
  // returns 1-based ordinal of term in this field
  int64_t startTerm(TermRef term) {
    docsFlushed = 0;
    termPackedBlockCount = 0;
    ttfAcc = 0;
    prevDocBlockLast = 0;  // each term's first doc block starts from base 0
    prevL1GroupLast = 0;
    l1GroupLastDoc = 0;
    l1GroupBlockCount = 0;
    l1GroupPackedBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
    l1GroupMinNorm = 0;
    l1BlockLastDocs.fill(0);
    l1BlockBodyOffsets.fill(0);
    l1GroupSurface.clear();
    group_output.resize(0);
    termSurface.clear();
    locOfPositionsForTerm = posOutput.size();
    pendingBlockPosByteOff = locOfPositionsForTerm;
    locOfDocsForTerm = docOutput.size();
    PackedTerm stored(termBytes.data() + termList.size() * PackedTerm::MAX_BYTES);
    term.copyTo(stored);
    termList.push_back(stored);
    return numTerms + (int64_t)termList.size();
  }

  void endTerm(TermRef term) {
    unused(term);
    auto docfreq = getDocFreq();
    // For fields that don't index freqs, ttf is *defined* as docfreq (each doc counts
    // once); the real occurrence count isn't stored and the reader reports docfreq, so
    // term-level and field-level (sumTotalTermFreq) stats must use the same definition.
    auto totalTermFreq = hasFreqs ? ttfAcc : (int64_t) docfreq;
    sumTotalTermFreq += totalTermFreq;
    if (docs.size() > 0) {
      assert(docs.back() < postingsWriter.getMaxDoc(space)); // sanity check to ensure we didn't go over provided maxDoc
    }
    if (totalTermFreq == 0) {
      // term with no documents, remove term we just added
      termList.pop_back();
      return;
    }
    // Only terms that reach the dictionary can stand in for long values.
    if (PackedTerm::mayBeFitted(std::string_view(termList.back()))) segFlags = FieldSegmentFlags::MAY_HAVE_FITTED;
    if (totalTermFreq == 1) {
      assert(getDocFileSize()==0 && docs.size()==1 && tfreqs.size()==1);
      assert(posdeltas.size() == (hasPositions ? 1u : 0u));
      int32_t blockTermOrd = (int32_t) termList.size() - 1;
      assert(blockTermOrd >= 0 && blockTermOrd < Postings::TERMS_BLOCK_SIZE);
      termPulsedMask |= 1u << (uint32_t) blockTermOrd;
      sumDocFreq += 1;
      if (hasFreqs && hasNorms) {
        // single-doc term: exact one-point frontier from its norm
        termSurface.clear();
        termSurface.raise(normForDoc(docs[0]), 1);
        appendImpactFrontier(termImpactRun, termSurface);
      }
      termPackedBlocks.push_back(0);
      termDocFreqs.push_back(1);
      termTtfCodes.push_back(0);
      termDocsEnd.push_back((uint64_t) (docOutput.size() - locOfDocsForTermBlock));
      if (hasPositions) {
        termPosOffsets.push_back((uint64_t) (posOutput.size() - locOfPositionsForTermBlock));
      }
      // the doc (+ position, if indexed) will be remembered to be included directly in the term dictionary (i.e. pulsing)
      pulsed.push_back((uint32_t) docs[0]);
      if (hasPositions) {
        pulsed.push_back((uint32_t) posdeltas.back());
        posdeltas.pop_back();
      }

      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);

    } else {
      // Finish positions that were not block encoded (only when positions are indexed)
      // TODO: possibly block encode positions across terms?  For that we would want ttf encoded in the terms dict (or could also store sum of all prev in docs file)
      //       Downside to this is that it could hurt block compression to mix terms (small + big deltas mixed)
      if (hasPositions) {
        for (auto posDelta : posdeltas) {
          // TODO: try group varint
          posOutput.writeVint(posDelta);
        }
      }
      posdeltas.resize(0);

      // Finish docs (and, for fields that index them, term freqs) that were not block encoded.
      // StreamVByte tail layout (see DocsEnumImpl::nextDoc):
      //   [docKeys][docData] followed, for fields with freqs, by [tfreqKeys][tfreqData].
      // Docs are d1 encoded; freqs are plain StreamVByte values.
      // TODO: when ttf==docfreq (all tf==1) the freq stream is constant 1 and can be dropped for
      //   scoring fields too (plan A); needs the freq blocks deferred/separated since flushDocs is eager.
      assert(docs.size() == tfreqs.size());
      const uint32_t n = (uint32_t) docs.size();
      if (n > 0) {
        const uint32_t kb = svbKeyBytes(n);
        uint8_t dkeys[Postings::DOCS_BLOCK_SIZE / 4 + 1];
        uint8_t ddata[Postings::DOCS_BLOCK_SIZE * 4];
        // d1 base: continue the cross-block delta from the last full block (0 when
        // the term is a single partial block), matching the docs codec convention.
        const uint32_t base = prevDocBlockLast;
        const uint32_t lastDoc = (uint32_t) docs.back();
        uint64_t tfSum = 0;
        uint32_t maxTf = 0;
        if (hasFreqs) {
          for (auto tf : tfreqs) {
            if (hasPositions) {
              tfSum += (uint32_t) tf;
            }
            maxTf = std::max(maxTf, (uint32_t) tf);
          }
        }
        uint32_t minNorm = buildImpactFrontier(n);
        uint8_t* ddEnd = svb_encode_scalar_d1_init((const uint32_t*) docs.data(), dkeys, ddata, n, base);
        compressed_output.resize(0);
        appendBytes(compressed_output, dkeys, kb);
        appendBytes(compressed_output, ddata, (size_t)(ddEnd - ddata));
        if (hasFreqs) {
          uint8_t tkeys[Postings::DOCS_BLOCK_SIZE / 4 + 1];
          uint8_t tdata[Postings::DOCS_BLOCK_SIZE * 4];
          uint8_t* tdEnd = svb_encode_scalar((const uint32_t*) tfreqs.data(), tkeys, tdata, n);
          appendBytes(compressed_output, tkeys, kb);
          appendBytes(compressed_output, tdata, (size_t)(tdEnd - tdata));
        }
        appendL0Block(lastDoc, base, compressed_output.size(), n, tfSum, maxTf, minNorm);
        prevDocBlockLast = lastDoc;
      }
      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);
      flushL1Group();


      sumDocFreq += docfreq;  // docfreq computed at the top of endTerm

      uint64_t ttfCode = 0;
      if (hasFreqs) {
        ttfCode = (uint64_t) (totalTermFreq - docfreq);
      }
      uint64_t posOffset = 0;
      if (hasPositions) {
        // offset from start of positions in term dict block
        posOffset = (uint64_t) (locOfPositionsForTerm - locOfPositionsForTermBlock);
      }

      termDocFreqs.push_back((uint32_t) docfreq);
      termTtfCodes.push_back(ttfCode);
      if (hasPositions) {
        termPosOffsets.push_back(posOffset);
      }
      if (hasFreqs && hasNorms) {
        appendImpactFrontier(termImpactRun, termSurface);
      }
      termPackedBlocks.push_back(termPackedBlockCount);
      termDocsEnd.push_back((uint64_t) (docOutput.size() - locOfDocsForTermBlock));
    }

    if (termList.size() == Postings::TERMS_BLOCK_SIZE) {
      flushTerms(false);
    }
  }

  RangeResult finishRange() {
    flushTerms(true);

    RangeResult result;
    result.termBlockOffsets = std::move(termBlockOffsets);
    result.runs = std::move(runs);
    result.separatorKeys = std::move(separatorKeys);
    result.firstTerm = std::move(firstTerm);
    if (numTerms != 0) {
      result.lastTerm.assign(lastTermOfPrevBlock.data(), lastTermOfPrevBlockLen);
    }
    result.sumDocFreq = sumDocFreq;
    result.sumTotalTermFreq = sumTotalTermFreq;
    result.dictionaryBytes = termOutput.size() - termsLoc;
    result.docsBytes = (uint64_t) (docOutput.size() - docsLoc);
    result.posBytes = (uint64_t) (posOutput.size() - posLoc);
    result.nTerms = numTerms;
    result.nBlocks = (int32_t) result.termBlockOffsets.size();
    result.segFlags = segFlags;
    assert((result.nTerms == 0) == (result.nBlocks == 0));
    return result;
  }

  // Appends the block offset table and trie. The trie extent includes the
  // slack required by the final dictionary block's SIMD decoders.
  static field_parts::Terms finishDictionary(
      PostingsWriter& writer, OutputStreamPtr&& stream, uint64_t start,
      std::span<const field_parts::Terms::BlockOffset> offsets, TrieBuilder& trie) {
    auto& out = *stream;
    auto dictionaryBytes = out.size() - start;
    auto root = trie.finish();
    const auto& bytes = trie.bytes();
    out.align(field_parts::Terms::OFFSETS_ALIGN);
    out.write(offsets.data(), offsets.size_bytes());
    out.write(bytes.data(), bytes.size());
    size_t suffixBytes = offsets.size_bytes() + bytes.size();
    size_t pad = suffixBytes < SVB_OVERREAD_PAD ? SVB_OVERREAD_PAD - suffixBytes : 0;
    static constexpr char zeros[SVB_OVERREAD_PAD] = {};
    if (pad) out.write(zeros, pad);
    assert(suffixBytes + pad >= SVB_OVERREAD_PAD);
    auto slice = writer.finishStream(std::move(stream));
    return {slice.location.addOffset(start), dictionaryBytes, bytes.size() + pad, root, {}};
  }

  // Stores the runs and dictionary bounds after the associated streams have locations.
  static void finishTermRuns(PostingsWriter& writer, WrittenTerms& written,
                             std::span<const field_parts::TermRun> runs, FieldType::Type type,
                             FieldType::flag_type flags, std::string_view first, std::string_view last) {
    written.terms.runs = writer.copyTermRuns(runs);
    if (field_parts::boundsKind(type, flags) == field_parts::BoundsKind::DICTIONARY) {
      written.bounds = writer.dictionaryBounds(first, last, type);
    }
  }

  // Finishes the serial field's dictionary and posting streams.
  [[nodiscard]] WrittenTerms endField() {
    assert(buildTrie);
    auto result = finishRange();

    WrittenTerms written{{result.nTerms, result.sumDocFreq, result.sumTotalTermFreq},
                         {}, result.segFlags};
    if (result.nTerms == 0) {
      assert(result.termBlockOffsets.empty());
      if (type == FieldType::ID && space == SpaceOrdinal::ROOT) {
        postingsWriter.idRange = IdRange{};
        postingsWriter.idDictionaryBlocks = 0;
      }
      dropIdFilter();
      return written;
    }

    written.terms = finishDictionary(postingsWriter, std::move(streams[0]), termsLoc,
                                     result.termBlockOffsets, trieBuilder);
    auto docs = postingsWriter.finishNonemptyStream(std::move(streams[1]));
    auto positions = postingsWriter.finishNonemptyStream(std::move(streams[2]));
    std::vector<field_parts::TermRun> fieldRuns;
    fieldRuns.reserve(result.runs.size());
    result.appendRuns(fieldRuns, 0, 0, 0, docs, positions);
    finishTermRuns(postingsWriter, written, fieldRuns, type, flags, result.firstTerm, result.lastTerm);
    if (type == FieldType::ID && space == SpaceOrdinal::ROOT) {
      const auto& bounds = std::get<field_parts::DictionaryBounds>(written.bounds);
      assert(bounds.lowerMode == field_parts::DictionaryBounds::Lower::EXACT
             && bounds.upperMode == field_parts::DictionaryBounds::Upper::INCLUSIVE);
      postingsWriter.idRange = IdRange{std::string(std::string_view(bounds.lower)), std::string(std::string_view(bounds.upper)), false};
      postingsWriter.idDictionaryBlocks = result.nBlocks;
      if (idFilter) written.idFilter = finishIdFilter(result.nTerms);
    }
    return written;
  }

private:
  // Writes the filter as its own slice. Every dictionary term is exactly one key.
  field_parts::Slice finishIdFilter(int64_t terms) {
    assert(idFilter);
    if (idFilter->numKeys() != (uint64_t)terms) throw std::logic_error("id filter keys differ from dictionary terms");
    // A static filter builds only now; charge what it holds meanwhile.
    auto building = (int64_t)idFilter->finishBytes();
    postingsWriter.accountRam(building);
    idFilterRam += building;
    auto bytes = idFilter->finish();
    auto stream = postingsWriter.getOutputStream();
    stream->write(bytes.data(), bytes.size());
    auto slice = postingsWriter.finishStream(std::move(stream));
    dropIdFilter();
    return slice;
  }

  void dropIdFilter() noexcept {
    if (!idFilter) return;
    postingsWriter.accountRam(-idFilterRam);
    idFilterRam = 0;
    idFilter.reset();
  }

public:

  void startDoc(int32_t doc) {
    unused(doc);
    curTf = 0;

    // This doc starts a new doc block: anchor the block to the position stream
    // before any of the doc's positions are added.  (All positioned paths -
    // Inverter's pushDocs and the merger's addDocsPos - route through here.)
    anchorPositionedDocBlock();
  }

  // Record a doc with a known term freq and no positions.  Positionless fields (string/id,
  // freq-only, and the positionless merge path) call this directly, since they already know
  // the freq and don't need the per-occurrence startDoc/addPositionDelta/endDoc dance.
  // endDoc() routes the position-streaming path here with its accumulated occurrence count.
  // tf is stored only when the field indexes freqs; for DOCS-only fields any tf>0 just
  // records the doc (its ttf is defined as docfreq).
  void addDoc(int32_t docid, int32_t tf) {
    if (tf == 0) {
      // TODO: revisit if this can happen... it depends on where deleted docs are checked.
      return;
    }
    docs.push_back(docid);
    tfreqs.push_back(tf);  // kept parallel to docs; only written when hasFreqs
    ttfAcc += tf;
    if (docs.size() == Postings::DOCS_BLOCK_SIZE) {
      flushDocs();
    }
  }

  void endDoc(int32_t doc) {
    addDoc(doc, curTf);
  }

  void endDoc(int32_t doc, int32_t expectedTf) {
    assert(curTf == expectedTf);
    endDoc(doc);
  }

  // Register one occurrence of the term in the current doc that carries a position.
  // The position is only stored when the field indexes positions; either way it
  // contributes to the term freq.
  void addPositionDelta(int32_t posDelta) {
    curTf++;
    if (hasPositions) {
      posdeltas.push_back(posDelta);
      if (posdeltas.size() == Postings::POSITIONS_BLOCK_SIZE) {
        flushPositions();
      }
    }
  }

  void appendPositionDeltas(std::span<const int32_t> deltas) {
    assert(hasPositions);
    curTf += (int32_t) deltas.size();
    appendPositionDeltasRaw(deltas);
  }

  // Copy positioned docs in source order while retaining ownership of output
  // doc-block anchors.  DeltaPull accepts a maximum count and may return
  // multiple spans before satisfying each output chunk's tf sum.
  template <typename DeltaPull>
  void addDocsWithPositions(std::span<const int32_t> docids,
                            std::span<const int32_t> tfValues,
                            DeltaPull&& deltaPull) {
    assert(hasPositions);
    assert(docids.size() == tfValues.size());
    size_t offset = 0;
    while (offset < docids.size()) {
      int32_t room = Postings::DOCS_BLOCK_SIZE - (int32_t) docs.size();
      int32_t count = std::min(room, (int32_t) (docids.size() - offset));
      assert(count > 0);

      anchorPositionedDocBlock();
      int64_t expectedDeltas = 0;
      for (int32_t i = 0; i < count; i++) {
        int32_t tf = tfValues[offset + (size_t) i];
        assert(tf > 0);
        expectedDeltas += (uint32_t) tf;
      }

      int64_t appendedDeltas = 0;
      while (appendedDeltas < expectedDeltas) {
        auto deltas = deltaPull(expectedDeltas - appendedDeltas);
        assert(!deltas.empty());
        assert((int64_t) deltas.size() <= expectedDeltas - appendedDeltas);
        appendPositionDeltasRaw(deltas);
        appendedDeltas += (int64_t) deltas.size();
      }
      assert(appendedDeltas == expectedDeltas);

      auto docChunk = docids.subspan(offset, (size_t) count);
      auto tfChunk = tfValues.subspan(offset, (size_t) count);
      docs.insert(docs.end(), docChunk.begin(), docChunk.end());
      tfreqs.insert(tfreqs.end(), tfChunk.begin(), tfChunk.end());
      ttfAcc += expectedDeltas;
      assert(docs.size() == tfreqs.size());
      assert(docs.size() <= Postings::DOCS_BLOCK_SIZE);
      if (docs.size() == Postings::DOCS_BLOCK_SIZE) {
        flushDocs();
      }
      offset += (size_t) count;
    }
  }

  void setNorms(TextNormsView norms) {
    this->norms = norms;
  }

};


class DocsWriter {
  ScreamingBuilder builder;
public:

  /// This writer currently *always* writes at least 2 bytes (the number of buckets) in the screaming bitset.
  /// Decisions should be made at a higher level to not use this for 0 or all-bits-set scenarios.
  /// This writer allocates from "pool" but does not rewind.  It is safe to release after finish() is called.
  DocsWriter(MemPool& pool, OutputStream& output) : builder(pool, output) {
  }

  // TODO: choose a smaller encoding at finish() for a small doc set or a low maxDoc.
  // The limit could depend on maxDoc, so
  // we could avoid using for small indexes / segments altogether.  DocsReader could encapsulate a bitset or
  // a list of decoded docs (and we could populate that from different formats such as the lowest-bit-is-tfreq)
  // used in termdoc postings.

  // Docs should be added in order!
  void addDoc(int32_t docid) {
    builder.add(docid);
  }

  // Same as addDoc... it's named startDoc target for DocStream.pushDocs.
  void startDoc(int32_t docid) {
    builder.add(docid);
  }

  uint64_t bitmapStart() const noexcept { return builder.bitmapStart(); }

  int32_t finish() {
    builder.flush();
    return builder.cardinality();
  }
};


class PresenceWriter {
  MemPool& pool;
  PostingsWriter& postingsWriter;
  SpaceOrdinal space;
  OutputStreamPtr idOutput;
  std::optional<DocsWriter> docsWriter;
  int32_t count = 0;

  void startBitmap() {
    idOutput = postingsWriter.getOutputStream();
    docsWriter.emplace(pool, *idOutput);
    for (int32_t doc = 0; doc < count; doc++) docsWriter->addDoc(doc);
  }

public:
  PresenceWriter(MemPool& pool, PostingsWriter& postingsWriter, SpaceOrdinal space)
      : pool(pool), postingsWriter(postingsWriter), space(space) {}

  // DocStream.pushDocs calls startDoc.
  void startDoc(int32_t docid) {
    if (!docsWriter && docid != count) startBitmap();
    if (docsWriter) docsWriter->addDoc(docid);
    count++;
  }

  int32_t numAdded() const { return count; }

  [[nodiscard]] WrittenPresence finish() {
    if (count == 0 || count == postingsWriter.getMaxDoc(space)) {
      assert(!docsWriter);
      return {count, {}};
    }
    // The gap may be at the end, after a dense prefix.
    if (!docsWriter) startBitmap();
    int32_t docsWithField = docsWriter->finish();
    auto start = docsWriter->bitmapStart();
    auto bytes = idOutput.pos() - start;
    auto slice = postingsWriter.finishStream(std::move(idOutput));
    return {docsWithField, slice.subSlice(start, bytes)};
  }

  // Writes the docs-with-field set from its docs in increasing order.
  template <class ForEachDoc>
  [[nodiscard]] static WrittenPresence write(MemPool& pool, PostingsWriter& postingsWriter,
                                             SpaceOrdinal space, int32_t docsWithField,
                                             ForEachDoc&& forEachDoc) {
    if (docsWithField == 0 || docsWithField == postingsWriter.getMaxDoc(space)) return {docsWithField, {}};
    auto guard = pool.rewindScopeGuard();
    PresenceWriter writer(pool, postingsWriter, space);
    forEachDoc([&](int32_t doc) { writer.startDoc(doc); });
    assert(writer.numAdded() == docsWithField);
    return writer.finish();
  }

};



} // end namespace
