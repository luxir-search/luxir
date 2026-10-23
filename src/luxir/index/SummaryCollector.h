// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Builds a segment's summary sidecar while the segment is written. Field
// writers feed values and terms into per-field collectors; PostingsWriter
// finalizes counts from the field directory and writes the file. Flush and
// merge share this code, so a merged segment's summary is recomputed from
// what the merge actually wrote.

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/schema/FieldType.h"
#include "luxir/schema/SummaryPolicy.h"
#include "luxir/store/Directory.h"
#include "luxir/util/BloomFilter.h"
#include "SegmentSummary.h"
#include "SummaryInfo.h"

namespace luxir {

// Collects the distinct terms of one field (or of one term range of a
// parallel merge) as filter keys and, while small, verbatim. Terms arrive in
// dictionary order. Memory is bounded: once more keys arrive than the
// filter budget can hold as a fuse filter, the keys fold into a Bloom filter
// of that budget and the key buffer is released.
class TermKeyCollector {
  SummaryPolicy policy;
  size_t maxKeys;
  std::vector<uint64_t> keys;
  std::optional<BloomFilter::Builder> bloom;
  std::vector<std::string> exact;
  bool exactOverflow = false;
  std::vector<std::vector<uint64_t>> prefixKeys;
  std::vector<std::string> lastPrefix;
  std::vector<bool> prefixDropped;
  uint64_t distinct = 0;

  void foldIntoBloom();

public:
  // Bits a fuse filter needs per key, used to size the key buffer.
  static constexpr uint64_t FUSE_BITS_PER_KEY = 9;
  static constexpr uint64_t BLOOM_SEED = 0x5f3759df9e3779b9ULL;

  explicit TermKeyCollector(SummaryPolicy policy);

  // Deterministic for a policy, so collectors of the same field (ranges of
  // one parallel merge) build OR-compatible Bloom filters.
  static BloomFilter::Header bloomPlan(const SummaryPolicy& policy);
  static size_t maxKeysFor(const SummaryPolicy& policy);

  void add(std::string_view term);
  // Appends the terms of `other`, all greater than this collector's terms.
  void append(TermKeyCollector&& other);

  uint64_t distinctTerms() const noexcept { return distinct; }
  size_t memoryBytes() const noexcept;

  // Emits this collector's payloads for `record`; `payloads` is the sidecar's
  // payload area under construction.
  void emit(OwnedFieldSummary& record, std::vector<std::byte>& payloads) const;
};

// Everything a segment summary records about one physical field.
class FieldSummaryCollector {
public:
  std::string fieldname;
  FieldType::Type type = FieldType::NONE;
  int32_t flags = 0;
  uint32_t typeFlags = 0;
  SummaryPolicy policy;

  uint64_t docsWithField = 0;
  uint64_t numValues = 0;
  bool hasNumValues = false;
  bool multiValued = false;
  uint64_t sumDocFreq = 0;
  uint64_t sumTotalTermFreq = 0;
  uint64_t nTerms = 0;

  int64_t numMin = std::numeric_limits<int64_t>::max();
  int64_t numMax = std::numeric_limits<int64_t>::min();
  bool hasNumeric = false;

  int32_t latMin = std::numeric_limits<int32_t>::max();
  int32_t latMax = std::numeric_limits<int32_t>::min();
  int32_t lonMin = std::numeric_limits<int32_t>::max();
  int32_t lonMax = std::numeric_limits<int32_t>::min();
  bool hasGeo = false;

  std::string termMin;
  std::string termMax;
  bool hasTermBounds = false;
  bool termBoundsTruncated = false;
  bool termMaxUnbounded = false;
  bool mayHaveFitted = false;
  std::optional<TermKeyCollector> terms;

  struct Vectors {
    uint32_t dims = 0;
    uint64_t count = 0;
    bool inconsistent = false;  // mixed dimensions: no statistics
    std::vector<double> sum;
    std::vector<double> boxMin;
    std::vector<double> boxMax;
  } vectors;

  // Distinct values of a column-only STRING field, for an exact set.
  std::vector<std::string> columnValues;
  bool columnValuesOverflow = false;

  FieldSummaryCollector(std::string_view name, FieldType::Type type, int32_t flags, SummaryPolicy policy)
      : fieldname(name), type(type), flags(flags), policy(std::move(policy)) {}

  void addInt64(int64_t encoded) noexcept {
    if (!policy.bounds) return;
    numMin = std::min(numMin, encoded);
    numMax = std::max(numMax, encoded);
    hasNumeric = true;
  }
  void addNumericBounds(int64_t min, int64_t max) noexcept {
    if (!policy.bounds) return;
    numMin = std::min(numMin, min);
    numMax = std::max(numMax, max);
    hasNumeric = true;
  }
  void addGeoPacked(int64_t packed) noexcept;
  // One vector's statistics; `accumulate` is the same for a caller that
  // gathers while indexing and hands the result over at flush.
  static void accumulate(Vectors& into, std::span<const float> vector);
  void addVector(std::span<const float> vector) { if (policy.bounds) accumulate(vectors, vector); }
  // A vector stored as raw little-endian float bytes.
  void addVectorBytes(std::string_view bytes);
  // A column-only string value, in any order and with repeats.
  void addColumnValue(std::string_view value);
  // The dictionary's first and last kept terms and the kept count.
  void setTermBounds(std::string_view first, std::string_view last, uint64_t nTerms);
  void setTermKeys(TermKeyCollector&& keys) { terms.emplace(std::move(keys)); }
  void setCounts(uint64_t docs, uint64_t values, bool hasValues, bool multi) noexcept {
    docsWithField = docs;
    numValues = values;
    hasNumValues = hasValues;
    multiValued = multi;
  }
  void setHeaderTotals(uint64_t docFreq, uint64_t totalTermFreq) noexcept {
    sumDocFreq = docFreq;
    sumTotalTermFreq = totalTermFreq;
  }

  size_t memoryBytes() const noexcept;
  // The field's record with its payloads appended to `payloads`.
  OwnedFieldSummary emit(std::vector<std::byte>& payloads) const;
};

// One segment's collectors and the sidecar they produce.
class SegmentSummaryBuilder {
  std::mutex mutex;
  std::deque<FieldSummaryCollector> collectors;
  boost::unordered_flat_map<std::string_view, FieldSummaryCollector*> byName;

public:
  // The collector of `name`, created on first use. Thread-safe: concurrent
  // field tasks of a merge register fields independently.
  FieldSummaryCollector& field(std::string_view name, FieldType::Type type, int32_t flags,
                               const SummaryPolicy& policy);
  FieldSummaryCollector* find(std::string_view name);
  size_t fieldCount();
  size_t memoryBytes();

  // The complete sidecar image.
  std::vector<std::byte> encode(uint64_t segId, int32_t maxDoc, bool complete);

  // Writes the sidecar as `fileName` and returns the summary it holds, with
  // the written file's descriptor.
  std::shared_ptr<const SegmentSummary> write(Directory& dir, std::string_view fileName, uint64_t segId,
                                              int32_t maxDoc, bool complete, FileDescriptor& descriptor);

  // Test hook: when set, PostingsWriter writes no sidecar (older-format fixtures).
  static inline bool disableForTests = false;
};

} // namespace luxir
