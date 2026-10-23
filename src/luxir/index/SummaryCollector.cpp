// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SummaryCollector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/reader/Postings.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/BinaryFuse8.h"
#include "luxir/util/TermHash.h"
#include "luxir/util/geo.h"

namespace luxir {

namespace {

using Flags = SegmentSummary::Flags;
using Kind = SegmentSummary::PayloadKind;

// Appends `bytes` to the payload area, 8-aligned, and records where it went.
OwnedPayload appendPayload(std::vector<std::byte>& payloads, Kind kind, std::span<const std::byte> bytes,
                           uint64_t count, uint64_t seed, uint32_t param) {
  size_t aligned = (payloads.size() + SegmentSummary::PAYLOAD_ALIGN - 1) & ~(SegmentSummary::PAYLOAD_ALIGN - 1);
  payloads.resize(aligned);
  OwnedPayload p;
  p.kind = kind;
  p.version = SegmentSummary::PAYLOAD_VERSION;
  p.offset = aligned;
  p.length = bytes.size();
  p.count = count;
  p.seed = seed;
  p.param = param;
  payloads.insert(payloads.end(), bytes.begin(), bytes.end());
  return p;
}

// EXACT_SET payload: u32 offsets[count] (relative to the payload), then
// u16 length + bytes per term, in sorted order.
std::vector<std::byte> encodeExactSet(std::span<const std::string> terms) {
  std::vector<std::byte> bytes(terms.size() * sizeof(uint32_t));
  for (size_t i = 0; i < terms.size(); i++) {
    uint32_t offset = (uint32_t)bytes.size();
    std::memcpy(bytes.data() + i * sizeof(uint32_t), &offset, sizeof(offset));
    uint16_t length = (uint16_t)std::min<size_t>(terms[i].size(), UINT16_MAX);
    size_t at = bytes.size();
    bytes.resize(at + sizeof(length) + length);
    std::memcpy(bytes.data() + at, &length, sizeof(length));
    std::memcpy(bytes.data() + at + sizeof(length), terms[i].data(), length);
  }
  return bytes;
}

} // namespace

// ---- TermKeyCollector ------------------------------------------------------

size_t TermKeyCollector::maxKeysFor(const SummaryPolicy& policy) {
  return std::max<size_t>(1, (size_t)(policy.filterMaxBytes * 8 / FUSE_BITS_PER_KEY));
}

BloomFilter::Header TermKeyCollector::bloomPlan(const SummaryPolicy& policy) {
  // The key count is unknown once the buffer overflowed; assume a few times
  // the buffer so k stays small and the filter degrades gracefully.
  return BloomFilter::plan(policy.filterMaxBytes, (uint64_t)maxKeysFor(policy) * 3, BLOOM_SEED);
}

TermKeyCollector::TermKeyCollector(SummaryPolicy p)
    : policy(std::move(p)), maxKeys(maxKeysFor(policy)),
      prefixKeys(policy.prefixes.size()), lastPrefix(policy.prefixes.size()),
      prefixDropped(policy.prefixes.size(), false) {}

void TermKeyCollector::foldIntoBloom() {
  if (bloom) return;
  bloom.emplace(bloomPlan(policy));
  for (uint64_t key : keys) bloom->add(key);
  keys.clear();
  keys.shrink_to_fit();
}

void TermKeyCollector::add(std::string_view term) {
  distinct++;
  if (policy.terms == SummaryPolicy::Terms::SET || policy.terms == SummaryPolicy::Terms::AUTO) {
    if (!exactOverflow) {
      if (exact.size() < policy.setMax) {
        exact.emplace_back(term);
      } else {
        exactOverflow = true;
        exact.clear();
        exact.shrink_to_fit();
      }
    }
  }
  bool wantFilter = policy.terms == SummaryPolicy::Terms::FILTER || policy.terms == SummaryPolicy::Terms::AUTO;
  if (wantFilter) {
    uint64_t key = summary::termKey(term);
    if (bloom) {
      bloom->add(key);
    } else {
      keys.push_back(key);
      if (keys.size() > maxKeys) foldIntoBloom();
    }
  }
  if (wantFilter || policy.terms == SummaryPolicy::Terms::SET) {
    for (size_t i = 0; i < policy.prefixes.size(); i++) {
      size_t length = policy.prefixes[i];
      if (prefixDropped[i] || term.size() < length) continue;
      std::string_view prefix = term.substr(0, length);
      if (!lastPrefix[i].empty() && lastPrefix[i] == prefix) continue;
      lastPrefix[i].assign(prefix);
      prefixKeys[i].push_back(summary::termKey(prefix));
      if (prefixKeys[i].size() > maxKeys) {
        prefixDropped[i] = true;
        prefixKeys[i].clear();
        prefixKeys[i].shrink_to_fit();
      }
    }
  }
}

void TermKeyCollector::append(TermKeyCollector&& other) {
  distinct += other.distinct;
  if (exactOverflow || other.exactOverflow || exact.size() + other.exact.size() > policy.setMax) {
    exactOverflow = true;
    exact.clear();
    exact.shrink_to_fit();
  } else {
    exact.insert(exact.end(), std::make_move_iterator(other.exact.begin()),
                 std::make_move_iterator(other.exact.end()));
  }
  if (bloom || other.bloom || keys.size() + other.keys.size() > maxKeys) {
    foldIntoBloom();
    if (other.bloom) bloom->merge(*other.bloom);
    for (uint64_t key : other.keys) bloom->add(key);
  } else {
    keys.insert(keys.end(), other.keys.begin(), other.keys.end());
  }
  for (size_t i = 0; i < prefixKeys.size() && i < other.prefixKeys.size(); i++) {
    if (prefixDropped[i] || other.prefixDropped[i]
        || prefixKeys[i].size() + other.prefixKeys[i].size() > maxKeys) {
      prefixDropped[i] = true;
      prefixKeys[i].clear();
      prefixKeys[i].shrink_to_fit();
      continue;
    }
    // Ranges are disjoint and ordered; a prefix shared across the boundary
    // is a duplicate key, which the filter build removes.
    prefixKeys[i].insert(prefixKeys[i].end(), other.prefixKeys[i].begin(), other.prefixKeys[i].end());
    lastPrefix[i] = other.lastPrefix[i];
  }
}

size_t TermKeyCollector::memoryBytes() const noexcept {
  size_t bytes = sizeof(*this) + keys.capacity() * sizeof(uint64_t);
  if (bloom) bytes += bloom->bytes();
  for (const auto& term : exact) bytes += term.capacity() + sizeof(std::string);
  for (const auto& keysOf : prefixKeys) bytes += keysOf.capacity() * sizeof(uint64_t);
  return bytes;
}

void TermKeyCollector::emit(OwnedFieldSummary& record, std::vector<std::byte>& payloads) const {
  using Terms = SummaryPolicy::Terms;
  if (policy.terms == Terms::NONE || distinct == 0) return;
  uint64_t budget = policy.filterMaxBytes;
  bool emitted = false;
  bool useSet = (policy.terms == Terms::SET || policy.terms == Terms::AUTO) && !exactOverflow;
  if (useSet) {
    record.payloads.push_back(appendPayload(payloads, Kind::EXACT_SET, encodeExactSet(exact), exact.size(), 0, 0));
    emitted = true;
  } else if (policy.terms == Terms::SET) {
    record.summaryFlags |= Flags::TERMS_OVERFLOW;
  }
  if (!emitted && (policy.terms == Terms::FILTER || policy.terms == Terms::AUTO)) {
    if (bloom) {
      std::vector<std::byte> bytes;
      bloom->appendTo(bytes);
      if (bytes.size() <= budget) {
        record.payloads.push_back(appendPayload(payloads, Kind::BLOOM, bytes, bloom->keys(),
                                                bloom->layout().seed, bloom->layout().k));
        budget -= bytes.size();
        emitted = true;
      }
    } else {
      std::vector<uint64_t> sorted = keys;
      std::vector<std::byte> bytes;
      if (BinaryFuse8::sizeForKeys(sorted.size()) <= budget && BinaryFuse8::build(sorted, bytes)) {
        auto header = BinaryFuse8::header(bytes);
        record.payloads.push_back(appendPayload(payloads, Kind::FUSE8, bytes, sorted.size(),
                                                header ? header->seed : 0, 0));
        budget -= bytes.size();
        emitted = true;
      }
    }
    if (!emitted) record.summaryFlags |= Flags::TERMS_OVERFLOW;
  }
  // Prefix filters share the remaining budget, shortest prefix first.
  for (size_t i = 0; i < policy.prefixes.size(); i++) {
    if (prefixDropped[i] || prefixKeys[i].empty()) continue;
    std::vector<uint64_t> sorted = prefixKeys[i];
    std::vector<std::byte> bytes;
    if (BinaryFuse8::sizeForKeys(sorted.size()) > budget || !BinaryFuse8::build(sorted, bytes)) continue;
    auto header = BinaryFuse8::header(bytes);
    record.payloads.push_back(appendPayload(payloads, Kind::PREFIX_FUSE8, bytes, sorted.size(),
                                            header ? header->seed : 0, policy.prefixes[i]));
    budget -= bytes.size();
  }
}

// ---- FieldSummaryCollector -------------------------------------------------

void FieldSummaryCollector::addGeoPacked(int64_t packed) noexcept {
  if (!policy.bounds) return;
  int32_t lat = geo::unpackLatitude(packed);
  int32_t lon = geo::unpackLongitude(packed);
  latMin = std::min(latMin, lat);
  latMax = std::max(latMax, lat);
  lonMin = std::min(lonMin, lon);
  lonMax = std::max(lonMax, lon);
  hasGeo = true;
}

void FieldSummaryCollector::accumulate(Vectors& vectors, std::span<const float> vector) {
  if (vectors.inconsistent) return;
  if (vectors.count == 0) {
    if (vector.empty() || vector.size() > UINT32_MAX) {
      vectors.inconsistent = true;
      return;
    }
    vectors.dims = (uint32_t)vector.size();
    vectors.sum.assign(vectors.dims, 0.0);
    vectors.boxMin.assign(vectors.dims, std::numeric_limits<double>::infinity());
    vectors.boxMax.assign(vectors.dims, -std::numeric_limits<double>::infinity());
  } else if (vector.size() != vectors.dims) {
    vectors.inconsistent = true;
    return;
  }
  for (uint32_t d = 0; d < vectors.dims; d++) {
    double v = vector[d];
    vectors.sum[d] += v;
    vectors.boxMin[d] = std::min(vectors.boxMin[d], v);
    vectors.boxMax[d] = std::max(vectors.boxMax[d], v);
  }
  vectors.count++;
}

void FieldSummaryCollector::addVectorBytes(std::string_view bytes) {
  if (!policy.bounds || bytes.size() % sizeof(float) != 0) return;
  // Column bytes are not necessarily float-aligned.
  std::vector<float> values(bytes.size() / sizeof(float));
  std::memcpy(values.data(), bytes.data(), bytes.size());
  accumulate(vectors, values);
}

void FieldSummaryCollector::addColumnValue(std::string_view value) {
  if (policy.terms != SummaryPolicy::Terms::SET || columnValuesOverflow) return;
  auto it = std::ranges::lower_bound(columnValues, value);
  if (it != columnValues.end() && *it == value) return;
  if (columnValues.size() >= policy.setMax) {
    columnValuesOverflow = true;
    columnValues.clear();
    columnValues.shrink_to_fit();
    return;
  }
  columnValues.insert(it, std::string(value));
}

void FieldSummaryCollector::setTermBounds(std::string_view first, std::string_view last, uint64_t nTerms) {
  this->nTerms = nTerms;
  if (!policy.bounds || nTerms == 0) return;
  termMin.assign(first.substr(0, SegmentSummary::TERM_BOUND_MAX_LEN));
  if (last.size() <= SegmentSummary::TERM_BOUND_MAX_LEN) {
    termMax.assign(last);
  } else {
    // A rounded-up prefix stays above every term that starts with it.
    auto next = SegmentSummary::successor(last.substr(0, SegmentSummary::TERM_BOUND_MAX_LEN));
    termMax = next ? std::move(*next) : std::string();
    if (!next) termMaxUnbounded = true;
  }
  termBoundsTruncated = first.size() > SegmentSummary::TERM_BOUND_MAX_LEN
      || last.size() > SegmentSummary::TERM_BOUND_MAX_LEN;
  hasTermBounds = true;
}

size_t FieldSummaryCollector::memoryBytes() const noexcept {
  size_t bytes = sizeof(*this) + fieldname.capacity() + termMin.capacity() + termMax.capacity()
      + (vectors.sum.capacity() + vectors.boxMin.capacity() + vectors.boxMax.capacity()) * sizeof(double);
  if (terms) bytes += terms->memoryBytes();
  return bytes;
}

OwnedFieldSummary FieldSummaryCollector::emit(std::vector<std::byte>& payloads) const {
  OwnedFieldSummary r;
  r.fieldname = fieldname;
  r.type = (uint32_t)type;
  r.flags = (uint32_t)flags;
  r.typeFlags = typeFlags;
  r.docsWithField = docsWithField;
  r.sumDocFreq = sumDocFreq;
  r.sumTotalTermFreq = sumTotalTermFreq;
  r.nTerms = nTerms;
  if (hasNumValues) {
    r.summaryFlags |= Flags::HAS_NUM_VALUES;
    r.numValues = numValues;
  }
  if (multiValued) r.summaryFlags |= Flags::MULTI_VALUED;
  if (mayHaveFitted) r.summaryFlags |= Flags::MAY_HAVE_FITTED;
  if (docsWithField == 0) {
    r.summaryFlags |= Flags::EMPTY;
    return r;
  }
  if (hasNumeric) {
    r.summaryFlags |= Flags::HAS_NUMERIC;
    r.numMin = numMin;
    r.numMax = numMax;
  }
  if (hasGeo) {
    r.summaryFlags |= Flags::HAS_GEO_BOX;
    r.geoMinLat = latMin;
    r.geoMaxLat = latMax;
    r.geoMinLon = lonMin;
    r.geoMaxLon = lonMax;
  }
  if (hasTermBounds) {
    r.summaryFlags |= Flags::HAS_TERM_BOUNDS;
    r.termMin = termMin;
    r.termMax = termMax;
    if (termBoundsTruncated) r.summaryFlags |= Flags::TERM_BOUNDS_TRUNCATED;
    if (termMaxUnbounded) r.summaryFlags |= Flags::TERM_MAX_UNBOUNDED;
  }
  if (terms) terms->emit(r, payloads);
  if (policy.terms == SummaryPolicy::Terms::SET && !terms) {
    if (columnValuesOverflow) {
      r.summaryFlags |= Flags::TERMS_OVERFLOW;
    } else if (!columnValues.empty()) {
      r.payloads.push_back(appendPayload(payloads, Kind::EXACT_SET, encodeExactSet(columnValues),
                                         columnValues.size(), 0, 0));
    }
  }
  if (policy.bounds && vectors.count > 0 && !vectors.inconsistent) {
    // Centroid in fp32; the radius bounds every vector's distance to that
    // stored centroid through the per-dimension box, rounded up past fp32.
    uint32_t dims = vectors.dims;
    std::vector<float> centroid(dims), boxMin(dims), boxMax(dims);
    double radiusSquared = 0.0;
    for (uint32_t d = 0; d < dims; d++) {
      centroid[d] = (float)(vectors.sum[d] / (double)vectors.count);
      boxMin[d] = std::nextafter((float)vectors.boxMin[d], -std::numeric_limits<float>::infinity());
      boxMax[d] = std::nextafter((float)vectors.boxMax[d], std::numeric_limits<float>::infinity());
      double c = centroid[d];
      double deviation = std::max(vectors.boxMax[d] - c, c - vectors.boxMin[d]);
      radiusSquared += deviation * deviation;
    }
    double radius = std::sqrt(radiusSquared) * (1.0 + std::ldexp(1.0, -20));
    float radius32 = std::nextafter((float)radius, std::numeric_limits<float>::infinity());
    std::vector<std::byte> bytes(24 + 3 * (size_t)dims * sizeof(float));
    std::memcpy(bytes.data(), &dims, sizeof(dims));
    std::memcpy(bytes.data() + 8, &vectors.count, sizeof(vectors.count));
    std::memcpy(bytes.data() + 16, &radius32, sizeof(radius32));
    std::memcpy(bytes.data() + 24, centroid.data(), dims * sizeof(float));
    std::memcpy(bytes.data() + 24 + dims * sizeof(float), boxMin.data(), dims * sizeof(float));
    std::memcpy(bytes.data() + 24 + 2 * dims * sizeof(float), boxMax.data(), dims * sizeof(float));
    r.payloads.push_back(appendPayload(payloads, Kind::VECTOR_CENTROID, bytes, vectors.count, 0, dims));
  }
  return r;
}

// ---- SegmentSummaryBuilder -------------------------------------------------

FieldSummaryCollector& SegmentSummaryBuilder::field(std::string_view name, FieldType::Type type, int32_t flags,
                                                    const SummaryPolicy& policy) {
  std::lock_guard lock(mutex);
  auto it = byName.find(name);
  if (it != byName.end()) return *it->second;
  auto& collector = collectors.emplace_back(name, type, flags, policy);
  byName.emplace(std::string_view(collector.fieldname), &collector);
  return collector;
}

FieldSummaryCollector* SegmentSummaryBuilder::find(std::string_view name) {
  std::lock_guard lock(mutex);
  auto it = byName.find(name);
  return it == byName.end() ? nullptr : it->second;
}

size_t SegmentSummaryBuilder::fieldCount() {
  std::lock_guard lock(mutex);
  return collectors.size();
}

size_t SegmentSummaryBuilder::memoryBytes() {
  std::lock_guard lock(mutex);
  size_t bytes = 0;
  for (const auto& c : collectors) bytes += c.memoryBytes();
  return bytes;
}

std::vector<std::byte> SegmentSummaryBuilder::encode(uint64_t segId, int32_t maxDoc, bool complete) {
  std::lock_guard lock(mutex);
  std::vector<const FieldSummaryCollector*> sorted;
  sorted.reserve(collectors.size());
  for (const auto& c : collectors) sorted.push_back(&c);
  std::ranges::sort(sorted, {}, [](const FieldSummaryCollector* c) -> std::string_view { return c->fieldname; });

  std::vector<std::byte> payloads;
  std::vector<OwnedFieldSummary> records;
  records.reserve(sorted.size());
  for (const auto* c : sorted) records.push_back(c->emit(payloads));

  std::pmr::monotonic_buffer_resource arena;
  api::SegmentSummaryInfo info;
  info.version = SegmentSummary::FORMAT_VERSION;
  info.hash_version = summary::HASH_VERSION;
  info.seg_id = segId;
  info.max_doc = maxDoc;
  info.complete = complete;
  info.fields = toWire(records, arena);
  std::vector<std::byte> section;
  if (!api::encode(info, section)) throw std::runtime_error("segment summary: encoding failed");
  if (section.size() > UINT32_MAX) throw std::runtime_error("segment summary: field records too large");

  size_t indexLen = (SegmentSummary::HEADER_BYTES + section.size() + SegmentSummary::PAYLOAD_ALIGN - 1)
      & ~(SegmentSummary::PAYLOAD_ALIGN - 1);
  std::vector<std::byte> image(indexLen + payloads.size());
  std::memcpy(image.data(), Postings::SUMMARY_HEADER.data(), Postings::SUMMARY_HEADER.size());
  uint32_t infoLen = (uint32_t)section.size();
  uint32_t indexLen32 = (uint32_t)indexLen;
  std::memcpy(image.data() + 8, &infoLen, sizeof(infoLen));
  std::memcpy(image.data() + 12, &indexLen32, sizeof(indexLen32));
  std::memcpy(image.data() + SegmentSummary::HEADER_BYTES, section.data(), section.size());
  std::memcpy(image.data() + indexLen, payloads.data(), payloads.size());
  return image;
}

std::shared_ptr<const SegmentSummary> SegmentSummaryBuilder::write(Directory& dir, std::string_view fileName,
                                                                   uint64_t segId, int32_t maxDoc, bool complete,
                                                                   FileDescriptor& descriptor) {
  auto image = std::make_shared<std::vector<std::byte>>(encode(segId, maxDoc, complete));
  Directory::FileCreateOptions options;
  options.expectedSize = image->size();
  auto file = dir.createFile(fileName, options);
  if (!file) throw FileIOException("Failed to create segment summary file " + std::string(fileName));
  OutputStream out(file.get());
  out.write(image->data(), image->size());
  out.close();
  dir.finishFile(*file);
  descriptor = file->descriptor();
  return SegmentSummary::fromBytes(std::move(image));
}

} // namespace luxir
