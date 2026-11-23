// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/IndexIVFRaBitQ.h>
#include <faiss/impl/io.h>
#include <faiss/impl/RaBitQUtils.h>
#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>
#include <faiss/utils/distances.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include "AuxReader.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/luxir_types.hpp"
#include "luxir/store/AcquireJoin.h"
#include "luxir/store/Directory.h"
#include "luxir/store/HeldRange.h"
#include "luxir/store/InputStream.h"
#include "luxir/util/log.h"
#include "luxir/util/luxir_util.h"
#include "luxir/util/Once.h"
#include "luxir/util/VectorRotation.h"

namespace luxir {

/// Fixed-width metadata for one vector aux index entry.  VectorIndexBuilder
/// packs this struct verbatim into AuxIndexInfo.opaque_meta; the reader copies
/// it back out.  Packed so the byte layout is the struct declaration, with no
/// implicit padding.  Native endianness: aux entries are rebuilt from the
/// segments they describe, never shipped between hosts.
LUXIR_UNALIGNED_START
struct VectorAuxMeta {
  static constexpr int32_t VERSION = 5;
  int32_t version = VERSION;
  int32_t dims = 0;
  int32_t metric = 0;       // raw luxir::api::VectorMetric value
  int32_t cosineNormalizeColumnOnRescore = 0;  // bool: raw cosine column, renorm at rescore
  int32_t engine = 0;       // ENGINE_IVFPQ or ENGINE_IVFRABITQ: the overlay's codec
  int32_t nlist = 0;
  int32_t pqM = 0;          // ENGINE_IVFPQ only
  int32_t pqBits = 0;       // ENGINE_IVFPQ only
  int32_t rabitqBits = 0;   // ENGINE_IVFRABITQ only: bits per dimension

  int32_t rotationKind = 0;  // ENGINE_IVFRABITQ only: VectorRotation kind
  uint64_t rotationSeed = 0; // ENGINE_IVFRABITQ only: VectorRotation seed
  // Length of the file's FAISS header (VectorAuxListsFooter). With nlist and
  // the file size it places the header, the list sizes and the footer, so a
  // reader acquires all three in one batch.
  int64_t faissHeaderBytes = 0;

  // IVF + product quantization; the FAISS header is a bare IndexIVFPQ.
  static constexpr int32_t ENGINE_IVFPQ = 1;
  // Bare IndexIVFRaBitQ in rotated space; queries use the recorded transform.
  static constexpr int32_t ENGINE_IVFRABITQ = 2;

  std::string toBytes() const {
    return std::string((const char*)this, sizeof(VectorAuxMeta));
  }

  /// Decode + validate.  Throws on size mismatch or unknown engine.
  static VectorAuxMeta fromBytes(std::string_view bytes, std::string_view auxName) {
    if (bytes.size() != sizeof(VectorAuxMeta)) {
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has invalid opaque_meta size {} (expected {})",
        auxName, bytes.size(), sizeof(VectorAuxMeta)));
    }
    VectorAuxMeta meta;
    std::memcpy(&meta, bytes.data(), sizeof(VectorAuxMeta));
    if (meta.version != VERSION) {
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has unsupported metadata version {}",
        auxName, (int32_t)meta.version));
    }
    if (meta.faissHeaderBytes <= 0 || meta.nlist <= 0) {
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has invalid header length {} or nlist {}",
        auxName, (int64_t)meta.faissHeaderBytes, (int32_t)meta.nlist));
    }
    if (meta.engine != ENGINE_IVFPQ && meta.engine != ENGINE_IVFRABITQ) {
      // (int32_t) copies: can't bind a reference to a packed field.
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has unsupported vector engine {}",
        auxName, (int32_t)meta.engine));
    }
    return meta;
  }
} LUXIR_UNALIGNED_END;

/// Default IVF coarse-list count for n vectors: the sqrt(n) rule clamped to
/// [1, 4096].  Single source of truth for both the builder's per-segment
/// build-time nlist default (VectorIndexBuilder::chooseIvfNList) and the query
/// side's merge-stable REFERENCE list count, which defines the nprobe scan
/// fraction "as if one IVF index of nlist=sqrt(N)".  Keeping them on one rule
/// (and one 4096 cap) means the search-side reference cannot silently drift
/// from how segments are actually partitioned.
inline int32_t defaultIvfNList(int64_t n) {
  if (n <= 0) return 1;
  int64_t derived = (int64_t)std::sqrt((double)n);
  derived = std::clamp<int64_t>(derived, 1, 4096);
  return (int32_t)std::min<int64_t>(derived, n);
}

static_assert(sizeof(VectorAuxMeta) == 10 * sizeof(int32_t) + sizeof(uint64_t) + sizeof(int64_t),
              "VectorAuxMeta layout is an on-disk contract");

// Intern by the disk parameters so each query rotates once per transform.
inline std::shared_ptr<const VectorRotation>
internVectorRotation(int32_t kind, int32_t dims, uint64_t seed) {
  static std::mutex mutex;
  static std::vector<std::weak_ptr<const VectorRotation>> entries;
  std::lock_guard<std::mutex> lock(mutex);
  std::erase_if(entries, [](const auto& w) { return w.expired(); });
  for (const auto& w : entries) {
    auto existing = w.lock();
    if (existing && existing->kind == kind && existing->dims == dims && existing->seed == seed) {
      return existing;
    }
  }
  auto shared = std::make_shared<const VectorRotation>(kind, dims, seed);
  entries.push_back(shared);
  return shared;
}

/// Trailing footer of a vector aux file.  The file layout (written by
/// VectorIndexBuilder, served by VectorAuxReader) keeps the FAISS blob down to
/// the index "header" (params + coarse quantizer + PQ codebooks + an empty
/// set of inverted lists) and stores the list payloads in Luxir's own layout
/// so they are served zero-copy from the file's memory view instead of being
/// deserialized into FAISS-owned RAM:
///
///   [0, H)    FAISS index blob (faiss::write_index with lists swapped empty)
///   [H8, S)   per-list sizes, int64[nlist]           (H8 = H padded to 8)
///   [S, I)    ids, int64[ntotal], concatenated in list order (8-aligned)
///   [I, C)    codes, uint8[ntotal * codeSize], same order
///   [C8, A)   amplitudes, float[ntotal * amplitudeCount], in list order
///   [.., end) this footer (8-aligned)
///
/// Native endianness, same contract as VectorAuxMeta: aux files are rebuilt
/// from the segments they describe, never shipped between hosts.
LUXIR_UNALIGNED_START
struct VectorAuxListsFooter {
  int64_t faissHeaderBytes = 0;
  int64_t nlist = 0;
  int64_t codeSize = 0;
  int64_t ntotal = 0;
  int32_t amplitudeCount = 0;  // PQ: 0; RaBitQ 1-bit: sigma1; multi-bit: sigmaB
  int32_t reserved = 0;
  int32_t version = 0;
  int32_t magic = 0;

  static constexpr int32_t MAGIC = 0x4C495853;  // "SXIL"
  static constexpr int32_t VERSION = 3;

  static constexpr size_t align8(size_t n) noexcept { return (n + 7) & ~(size_t)7; }

  /// Section offsets within the file, derived from the footer fields.
  size_t sizesOffset() const noexcept { return align8((size_t)faissHeaderBytes); }
  size_t idsOffset() const noexcept { return sizesOffset() + (size_t)nlist * 8; }
  size_t codesOffset() const noexcept { return idsOffset() + (size_t)ntotal * 8; }
  size_t amplitudesOffset() const noexcept {
    return align8(codesOffset() + (uint64_t)ntotal * (uint64_t)codeSize);
  }
  size_t expectedFileBytes() const noexcept {
    // Unsigned multiply: ntotal/codeSize are file-controlled (read from the
    // footer before validation), so a corrupt footer could overflow a signed
    // int64 product (UB) inside the very check meant to reject it.
    return align8(amplitudesOffset() + (uint64_t)ntotal * (uint32_t)amplitudeCount * sizeof(float))
         + sizeof(VectorAuxListsFooter);
  }

  /// Decode + validate the footer at the tail of a fileBytes-long file.
  static VectorAuxListsFooter fromFileTail(const char* base, size_t fileBytes,
                                           std::string_view auxName) {
    checkFits(fileBytes, auxName);
    return decode(base + fileBytes - sizeof(VectorAuxListsFooter), fileBytes, auxName);
  }

  static void checkFits(size_t fileBytes, std::string_view auxName) {
    if (fileBytes < sizeof(VectorAuxListsFooter)) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' file is {} bytes, smaller than the lists footer",
        auxName, fileBytes));
    }
  }

  /// Decode + validate a footer read from the tail of a fileBytes-long file.
  static VectorAuxListsFooter decode(const char* bytes, size_t fileBytes, std::string_view auxName) {
    checkFits(fileBytes, auxName);
    VectorAuxListsFooter f;
    std::memcpy(&f, bytes, sizeof(f));
    if (f.magic != MAGIC) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' is not in the header+lists layout (bad magic)", auxName));
    }
    if (f.version != VERSION) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' has unsupported lists layout version {}",
        auxName, (int32_t)f.version));
    }
    size_t offset = 0, payload = fileBytes - sizeof(f);
    auto section = [&](uint64_t count, uint64_t width) {
      if (width == 0) return true;
      if (count > (payload - offset) / width) return false;
      offset += count * width;
      return true;
    };
    auto align = [&]() { return section((8 - offset % 8) % 8, 1); };
    // Consume the available bytes before multiplying or adding file-controlled
    // values. This also rejects overlapping sections and alignment overflow.
    if (f.faissHeaderBytes <= 0 || f.nlist <= 0 || f.codeSize <= 0 || f.ntotal < 0
        || f.amplitudeCount < 0 || f.amplitudeCount > 1 || f.reserved != 0
        || !section(f.faissHeaderBytes, 1) || !align()
        || !section(f.nlist, 8) || !section(f.ntotal, 8)
        || !section(f.ntotal, f.codeSize) || !align()
        || !section(f.ntotal, (uint32_t)f.amplitudeCount * sizeof(float)) || !align()
        || offset != payload) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' lists footer does not match file size {}",
        auxName, fileBytes));
    }
    return f;
  }
} LUXIR_UNALIGNED_END;

static_assert(sizeof(VectorAuxListsFooter) == 4 * sizeof(int64_t) + 4 * sizeof(int32_t),
              "VectorAuxListsFooter layout is an on-disk contract");

// Relative to the factors following the sign bytes. The derived FAISS type
// is not standard-layout, so offsetof(SignBitFactorsWithError, f_error) is
// not portable. Its packed base and trailing float define the wire layout.
inline constexpr size_t RABITQ_F_ERROR_OFFSET = sizeof(faiss::rabitq_utils::SignBitFactors);
static_assert(RABITQ_F_ERROR_OFFSET == 2 * sizeof(float));
static_assert(sizeof(faiss::rabitq_utils::SignBitFactorsWithError) == RABITQ_F_ERROR_OFFSET + sizeof(float));

/// Read-only faiss::InvertedLists over the list sections of a vector overlay
/// file (VectorAuxListsFooter). On a file whose holds are free (RAM, mmap),
/// lists point into the file's view. Otherwise get_ids / get_codes hold the
/// list's range until the matching release_ids / release_codes, so a scan
/// holds only the lists it reads, and only while it reads them. A scan that
/// knows its lists holds them first with retain(), one batch per file; the
/// gets inside the batch then find them held. Holds are shared and counted
/// across concurrent scans. ReadOnlyInvertedLists supplies throwing
/// add/update/resize.
class HeldInvertedLists final : public faiss::ReadOnlyInvertedLists {
  struct Held {
    HeldRange range;
    size_t refs = 0;
  };

  InputFile& file;
  HeldRange view;                    // the whole file, when its holds are free
  std::array<size_t, 3> sectionOffsets;
  std::array<size_t, 3> rowBytes;    // 0: the codec has no such section
  std::vector<size_t> offsets;       // prefix sums of list sizes, nlist+1 entries
  std::unique_ptr<std::atomic<uint8_t>[]> amplitudesChecked;
  mutable std::mutex mutex;
  mutable boost::unordered_flat_map<uint64_t, Held> held;

public:
  enum Section : uint8_t { IDS, CODES, AMPLITUDES };

  /// Lists a scan holds, released when it is destroyed.
  class Batch {
    friend class HeldInvertedLists;
    const HeldInvertedLists* lists = nullptr;
    std::vector<uint64_t> keys;

  public:
    Batch() = default;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    ~Batch() {
      for (uint64_t key : keys) lists->release(key);
    }
  };

  /// One list's amplitudes, held and validated, until destroyed.
  class Amplitudes {
    const HeldInvertedLists& lists;
    size_t list;
    std::span<const float> values;

  public:
    Amplitudes(const HeldInvertedLists& lists, size_t list) : lists(lists), list(list) {
      values = {(const float*)lists.hold(list, AMPLITUDES), lists.list_size(list)};
      try {
        lists.validateAmplitudes(list, values);
      } catch (...) {
        lists.release(list, AMPLITUDES);
        throw;
      }
    }
    Amplitudes(const Amplitudes&) = delete;
    Amplitudes& operator=(const Amplitudes&) = delete;
    ~Amplitudes() { lists.release(list, AMPLITUDES); }
    const float* data() const noexcept { return values.data(); }
    std::span<const float> span() const noexcept { return values; }
  };

private:
  static uint64_t keyOf(size_t list, Section section) noexcept { return (uint64_t)list * 4 + section; }

  const char* find(uint64_t key) const {
    auto it = held.find(key);
    if (it == held.end()) return nullptr;
    it->second.refs++;
    return it->second.range.getData();
  }

  // Installs a hold acquired outside the lock, or counts the one another
  // scan installed first; the caller drops `range` after unlocking.
  const char* install(uint64_t key, HeldRange& range) const {
    auto [it, inserted] = held.try_emplace(key);
    if (inserted) it->second.range = std::move(range);
    it->second.refs++;
    return it->second.range.getData();
  }

  void release(uint64_t key) const noexcept {
    HeldRange dropped;
    {
      std::lock_guard lock(mutex);
      auto it = held.find(key);
      assert(it != held.end() && it->second.refs > 0);
      if (it == held.end() || --it->second.refs > 0) return;
      dropped = std::move(it->second.range);
      held.erase(it);
    }
  }

  const char* hold(size_t list, Section section) const {
    if (list_size(list) == 0) return nullptr;
    ByteRange range = rangeOf(list, section);
    if (holdsFree()) return view.getData() + range.offset;
    uint64_t key = keyOf(list, section);
    {
      std::lock_guard lock(mutex);
      if (const char* data = find(key)) return data;
    }
    HeldRange acquired = file.acquire(range);
    std::lock_guard lock(mutex);
    return install(key, acquired);
  }

  void release(size_t list, Section section) const noexcept {
    if (holdsFree() || list_size(list) == 0) return;
    release(keyOf(list, section));
  }

  void validateAmplitudes(size_t list, std::span<const float> values) const {
    if (amplitudesChecked[list].load(std::memory_order_relaxed)) return;
    for (float value : values) {
      if (!std::isfinite(value) || value < 0) throw std::runtime_error("VectorAuxReader: invalid RaBitQ amplitude");
    }
    amplitudesChecked[list].store(1, std::memory_order_relaxed);
  }

public:
  HeldInvertedLists(InputFile& file, size_t fileBytes, const VectorAuxListsFooter& footer, const int64_t* listSizes)
      : faiss::ReadOnlyInvertedLists((size_t)footer.nlist, (size_t)footer.codeSize), file(file),
        sectionOffsets{footer.idsOffset(), footer.codesOffset(), footer.amplitudesOffset()},
        rowBytes{sizeof(faiss::idx_t), (size_t)footer.codeSize, (size_t)footer.amplitudeCount * sizeof(float)} {
    if (file.holdsFree()) view = file.acquire({0, fileBytes});
    offsets.resize(nlist + 1);
    offsets[0] = 0;
    for (size_t i = 0; i < nlist; i++) {
      offsets[i + 1] = offsets[i] + (size_t)listSizes[i];
    }
    if (rowBytes[AMPLITUDES]) amplitudesChecked = std::make_unique<std::atomic<uint8_t>[]>(nlist);
  }

  /// True when lists point into the file's view: nothing to hold or batch.
  bool holdsFree() const noexcept { return view.getData() != nullptr; }
  /// The file, for read-ahead advice on a free view; null otherwise.
  InputFile* viewFile() const noexcept { return holdsFree() ? &file : nullptr; }
  bool hasAmplitudes() const noexcept { return rowBytes[AMPLITUDES] != 0; }
  size_t totalSize() const noexcept { return offsets.back(); }

  ByteRange rangeOf(size_t list, Section section) const noexcept {
    size_t width = rowBytes[section];
    return {sectionOffsets[section] + offsets[list] * width, (offsets[list + 1] - offsets[list]) * width};
  }

  size_t list_size(size_t listNo) const override {
    return offsets[listNo + 1] - offsets[listNo];
  }
  const uint8_t* get_codes(size_t listNo) const override {
    return (const uint8_t*)hold(listNo, CODES);
  }
  const faiss::idx_t* get_ids(size_t listNo) const override {
    return (const faiss::idx_t*)hold(listNo, IDS);
  }
  void release_codes(size_t listNo, const uint8_t*) const override { release(listNo, CODES); }
  void release_ids(size_t listNo, const faiss::idx_t*) const override { release(listNo, IDS); }

  /// Holds `sections` of `lists` into `into`. Ranges not yet held are
  /// acquired as one batch on `join`, which the caller waits on; `into` must
  /// outlive that wait. Empty lists and lists of a free view need nothing.
  void retain(std::span<const faiss::idx_t> lists, std::initializer_list<Section> sections, Batch& into,
              AcquireJoin& join) const {
    into.lists = this;
    if (holdsFree()) return;
    std::vector<uint64_t> missing;
    std::vector<ByteRange> ranges;
    {
      std::lock_guard lock(mutex);
      for (auto list : lists) {
        if (list < 0 || list_size((size_t)list) == 0) continue;
        for (auto section : sections) {
          if (!rowBytes[section]) continue;
          uint64_t key = keyOf((size_t)list, section);
          if (find(key)) into.keys.push_back(key);
          else {
            missing.push_back(key);
            ranges.push_back(rangeOf((size_t)list, section));
          }
        }
      }
    }
    if (missing.empty()) return;
    join.acquire(file, ranges, [this, missing = std::move(missing), &into](std::vector<HeldRange>&& holds) {
      {
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < missing.size(); i++) {
          install(missing[i], holds[i]);
          into.keys.push_back(missing[i]);
        }
      }
      holds.clear();
    });
  }
};

/// AuxReader for kind == "vector_faiss".  Holds a deserialized faiss::Index
/// lazily decoded from the FAISS header of the file referenced by
/// AuxIndexInfo.files(0), plus the VectorAuxMeta decoded out of
/// AuxIndexInfo.opaque_meta.  The inverted lists stay in the file.
///
/// Contract (matches VectorIndexBuilder): one IVF file (IVF+RaBitQ or IVF+PQ) per segment
/// overlay.
/// Both single- and multi-valued vector fields are supported: every vector is
/// its own segment-local FAISS id, and KnnQuery maps each id back via the
/// valueRank->docId column.  The file is opened at IndexReader open time so a
/// reader can keep serving an unlinked overlay file; faiss::read_index runs on
/// first kNN use and is cached.
class VectorAuxReader : public AuxReader {
  std::string name;
  std::string field;
  std::shared_ptr<InputFile> file;
  // The FAISS header, until getFaissIndex decodes it.
  mutable HeldRange header;
  mutable OnceFlag loadOnce;
  // Declared before faissIndex so the index is destroyed first; the index
  // never owns the lists (replace_invlists own=false).
  std::unique_ptr<HeldInvertedLists> lists;
  mutable std::unique_ptr<faiss::Index> faissIndex;
  // ENGINE_IVFRABITQ: the random rotation the IVF index was built behind
  // (shared across overlays by internVectorRotation); null otherwise.
  mutable std::shared_ptr<const VectorRotation> rotation;
  VectorAuxMeta meta;
  VectorAuxListsFooter footer;
  mutable std::mutex unitCountMutex;
  mutable std::vector<std::pair<std::string, int64_t>> unitCounts;

public:
  static constexpr std::string_view KIND = "vector_faiss";

  static bool usePrecomputedTable(faiss::MetricType metric, bool byResidual,
                                  int64_t nlist, int64_t m, int64_t ksub, int64_t ntotal, int32_t dims) {
    // Bound the L2 residual table by the raw vector size, in floats.
    return metric == faiss::METRIC_L2 && byResidual && (double)nlist * m * ksub <= (double)ntotal * dims;
  }

  /// Acquires the FAISS header, the list sizes and the footer in one batch:
  /// the footer is fixed-size at the end of the fileBytes-long file, and the
  /// metadata places the other two. Lists are held only while scans read
  /// them (HeldInvertedLists).
  VectorAuxReader(std::string name, std::string field,
                  uint64_t gen, uint64_t builtCoreGen,
                  std::shared_ptr<InputFile> file, size_t fileBytes,
                  const VectorAuxMeta& meta)
    : AuxReader(gen, builtCoreGen),
      name(std::move(name)), field(std::move(field)),
      file(std::move(file)), meta(meta) {
    VectorAuxListsFooter::checkFits(fileBytes, this->name);
    size_t footerAt = fileBytes - sizeof(VectorAuxListsFooter);
    size_t sizesAt = VectorAuxListsFooter::align8((size_t)meta.faissHeaderBytes);
    size_t sizesBytes = (size_t)meta.nlist * sizeof(int64_t);
    if ((uint64_t)meta.faissHeaderBytes > footerAt || sizesAt > footerAt || sizesBytes > footerAt - sizesAt) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' header of {} bytes and {} lists do not fit file size {}",
        this->name, (int64_t)meta.faissHeaderBytes, (int32_t)meta.nlist, fileBytes));
    }
    ByteRange ranges[] = {{0, (uint64_t)meta.faissHeaderBytes, ReadClass::METADATA},
                          {sizesAt, sizesBytes, ReadClass::METADATA},
                          {footerAt, sizeof(VectorAuxListsFooter), ReadClass::METADATA}};
    auto holds = this->file->acquireAll(ranges);
    footer = VectorAuxListsFooter::decode(holds[2].getData(), fileBytes, this->name);
    if (footer.faissHeaderBytes != meta.faissHeaderBytes || footer.nlist != meta.nlist) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' lists footer (header {} bytes, nlist {}) does not match "
        "opaque_meta (header {} bytes, nlist {})", this->name, (int64_t)footer.faissHeaderBytes,
        (int64_t)footer.nlist, (int64_t)meta.faissHeaderBytes, (int32_t)meta.nlist));
    }
    if (footer.amplitudeCount != (isRaBitQ() ? 1 : 0)) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' amplitude layout does not match codec", this->name));
    }
    const int64_t* sizes = (const int64_t*)holds[1].getData();
    int64_t remaining = footer.ntotal;
    for (int64_t i = 0; i < footer.nlist; i++) {
      if (sizes[i] < 0 || sizes[i] > remaining) {
        throw std::runtime_error("VectorAuxReader: invalid list size");
      }
      remaining -= sizes[i];
    }
    if (remaining != 0) throw std::runtime_error("VectorAuxReader: list sizes do not match ntotal");
    // Amplitudes are validated per list, on its first scan (HeldInvertedLists).
    lists = std::make_unique<HeldInvertedLists>(*this->file, fileBytes, footer, sizes);
    header = std::move(holds[0]);
  }

  // Physical populations depend on the immutable source column and ownership,
  // not on a query domain or a live-docs generation. One scalar per ancestor.
  template <class Count> int64_t unitsWithValue(std::string_view ancestor, Count count) const {
    std::lock_guard lock(unitCountMutex);
    for (const auto& [path, units] : unitCounts) if (path == ancestor) return units;
    int64_t units = count();
    unitCounts.emplace_back(ancestor, units);
    return units;
  }

  std::string_view getKind() const override { return KIND; }
  std::string_view getName() const override { return name; }
  std::string_view getField() const noexcept { return field; }
  int32_t getDims() const noexcept { return meta.dims; }
  int32_t getMetric() const noexcept { return meta.metric; }
  bool shouldNormalizeColumnOnCosineRescore() const noexcept {
    return meta.cosineNormalizeColumnOnRescore != 0;
  }
  int32_t getEngine() const noexcept { return meta.engine; }
  bool isRaBitQ() const noexcept { return meta.engine == VectorAuxMeta::ENGINE_IVFRABITQ; }
  int32_t getNList() const noexcept { return meta.nlist; }
  int32_t getPqM() const noexcept { return meta.pqM; }
  int32_t getPqBits() const noexcept { return meta.pqBits; }
  int32_t getRaBitQBits() const noexcept { return meta.rabitqBits; }
  bool scoresAreExact() const noexcept { return false; }

  /// The rotation queries must pass through before reaching getFaissIndex()
  /// (ENGINE_IVFRABITQ), or null.  Valid after getFaissIndex().
  const VectorRotation* getRotation() const noexcept { return rotation.get(); }
  /// The index's inverted lists.
  const HeldInvertedLists& getLists() const noexcept { return *lists; }

  // Search-time entry point: the IVF index (for ENGINE_IVFRABITQ, the index
  // behind the rotation; see getRotation).  Decode is lazy and once-only;
  // faiss::Index::search is thread-safe for concurrent readers after
  // construction.  Only the FAISS header (params + coarse quantizer + PQ
  // codebooks) is deserialized into RAM; scans read the inverted lists
  // through HeldInvertedLists (see VectorAuxListsFooter).
  faiss::Index* getFaissIndex() const {
    callOnce(loadOnce, [this]() {
      BufferIOReader io(header.getData(), (size_t)footer.faissHeaderBytes);
      std::unique_ptr<faiss::Index> idx(faiss::read_index(&io, faiss::IO_FLAG_SKIP_PRECOMPUTE_TABLE));
      if (!idx) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: read_index returned null for {}", name));
      }
      if (idx->d != meta.dims) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: dims mismatch for aux '{}': index has {}, opaque_meta says {}",
          name, (int)idx->d, (int32_t)meta.dims));
      }
      std::shared_ptr<const VectorRotation> rot;
      if (isRaBitQ()) {
        auto* rq = dynamic_cast<faiss::IndexIVFRaBitQ*>(idx.get());
        if (rq == nullptr) {
          throw std::runtime_error(std::format(
            "VectorAuxReader: aux '{}' header is not an IVF+RaBitQ index", name));
        }
        if (rq->rabitq.nb_bits != (size_t)meta.rabitqBits) {
          throw std::runtime_error(std::format(
            "VectorAuxReader: RaBitQ bits mismatch for aux '{}': index has {}, opaque_meta says {}",
            name, rq->rabitq.nb_bits, (int32_t)meta.rabitqBits));
        }
        if (meta.rotationKind != VectorRotation::FHT_KAC) {
          throw std::runtime_error(std::format(
            "VectorAuxReader: aux '{}' has unsupported rotation kind {}",
            name, (int32_t)meta.rotationKind));
        }
        rot = internVectorRotation(meta.rotationKind, meta.dims, meta.rotationSeed);
      }
      auto* ivf = dynamic_cast<faiss::IndexIVF*>(idx.get());
      if (ivf == nullptr) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' header is not an IVF index", name));
      }
      if (ivf->nlist != (size_t)meta.nlist) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: nlist mismatch for aux '{}': index has {}, opaque_meta says {}",
          name, ivf->nlist, (int32_t)meta.nlist));
      }
      if ((int64_t)ivf->nlist != footer.nlist
          || (int64_t)ivf->code_size != footer.codeSize
          || ivf->ntotal != footer.ntotal) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' lists footer (nlist={} codeSize={} ntotal={}) "
          "does not match index header (nlist={} codeSize={} ntotal={})",
          name, (int64_t)footer.nlist, (int64_t)footer.codeSize, (int64_t)footer.ntotal,
          ivf->nlist, ivf->code_size, ivf->ntotal));
      }

      if (auto* pq = dynamic_cast<faiss::IndexIVFPQ*>(idx.get())) {
        pq->use_precomputed_table = -1;
        if (usePrecomputedTable(pq->metric_type, pq->by_residual,
                                pq->nlist, pq->pq.M, pq->pq.ksub, pq->ntotal, pq->d)) {
          pq->use_precomputed_table = 1;
          pq->precompute_table();
        }
      }

      if ((int64_t)lists->totalSize() != footer.ntotal) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' list sizes sum to {}, expected ntotal {}",
          name, lists->totalSize(), (int64_t)footer.ntotal));
      }
      // Drops the empty ArrayInvertedLists the header deserialized with; the
      // index does not own the held lists (this reader does).
      ivf->replace_invlists(lists.get(), /*own=*/false);
      faissIndex = std::move(idx);
      rotation = std::move(rot);
      header = HeldRange();
    });
    return faissIndex.get();
  }

  /// Open the FAISS index referenced by `info`.  Returns nullptr when the
  /// file is missing and missingFileOK is true (caller should re-parse
  /// IndexInfo and retry).
  static std::shared_ptr<VectorAuxReader> open(Directory& dir,
                                               const luxir::api::AuxIndexInfo& info,
                                               bool missingFileOK) {
    // V1 layout: exactly one file per vector aux entry.
    if (info.files.size() != 1) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: expected 1 file, got {} for aux '{}'",
        info.files.size(), info.name));
    }
    std::string_view fname = info.files[0].name;

    // expectSynced=true: aux files were fsynced before the IndexInfo that
    // references them was published, same contract as segment files.
    auto file = dir.openFile(fname, /*expectSynced=*/true, info.files[0].size);
    if (file == nullptr) {
      if (missingFileOK) {
        LOG_TRACE("VectorAuxReader::open: file {} missing for aux '{}', will retry",
                  fname, info.name);
        return nullptr;
      }
      throw std::filesystem::filesystem_error(
        std::format("Missing aux index file '{}' for aux '{}'", fname, info.name),
        std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return open(std::move(file), info);
  }

  /// Opens the reader over the entry's file, already open.
  static std::shared_ptr<VectorAuxReader> open(std::shared_ptr<InputFile> file,
                                               const luxir::api::AuxIndexInfo& info) {
    if (info.files.size() != 1) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: expected 1 file, got {} for aux '{}'",
        info.files.size(), info.name));
    }
    VectorAuxMeta meta = VectorAuxMeta::fromBytes(std::string_view((const char*)info.opaque_meta.data(), info.opaque_meta.size()), info.name);

    // Residency rule: index data must NOT be force-resident.  IVF/IVF+PQ list
    // payloads are read from the file through HeldInvertedLists (zero-copy
    // from a free view, per-list holds otherwise); only the small routing
    // model (centroids + PQ codebooks) is deserialized into RAM.
    //
    // Stay-on-disk story for other index types when we add them:
    //   - HNSW: FAISS 1.14.1's IO_FLAG_MMAP_IFC can mmap both the graph
    //     adjacency (hnsw.neighbors) and the vectors (IndexFlat.codes), copying
    //     only ~12 B/vector of per-node metadata (levels/offsets) into RAM.
    //     Caveat: random-access graph traversal still churns its working
    //     set under memory pressure (the durable cost is the access pattern, not
    //     forced residency), so mmap helps HNSW less than it helps the sequential
    //     IVF / IVF+PQ list scans.
    //   - PQ standalone, NSG, NN-Descent: in-memory; same IO_FLAG_MMAP_IFC
    //     route as HNSW when they matter.
    //
    // Likely tiering once past v1: brute-force-over-column for small segments,
    // HNSW for RAM-fit mid-size, IVF + mmap lists for large.
    // The manifest records the size, so placing the footer reads nothing.
    size_t fileBytes = info.files[0].size ? (size_t)info.files[0].size : file->size();
    return std::make_shared<VectorAuxReader>(
      std::string(info.name), std::string(info.field),
      info.gen, info.built_core_gen, std::move(file), fileBytes, meta);
  }

private:
  /// faiss::IOReader backed by a contiguous in-memory buffer.  Our Directory
  /// implementations expose the whole file as a memory view (mmap for FSDir,
  /// owned buffer for RAMDir), so we can hand FAISS a sequential reader
  /// without copying.  faiss::read_index copies what it needs into the
  /// returned Index, so the buffer doesn't need to outlive the call.
  class BufferIOReader : public faiss::IOReader {
    const char* data;
    size_t size;
    size_t pos = 0;

  public:
    BufferIOReader(const char* d, size_t s) : data(d), size(s) {}

    size_t operator()(void* ptr, size_t itemSize, size_t nitems) override {
      if (itemSize == 0) return nitems;
      size_t avail = (size - pos) / itemSize;
      size_t toRead = std::min(nitems, avail);
      std::memcpy(ptr, data + pos, toRead * itemSize);
      pos += toRead * itemSize;
      return toRead;
    }
  };
};

} // namespace luxir
