// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Field loading + streaming response emission for an already-ranked list of
// (segdoc, score) pairs.  Used by TopDocsReq (after collector sort) and by
// FusionOp (after RRF merge); both share the same segment-grouped column
// loader, batched assembly, and streaming reply path.
//
// Response columns are built NON-OWNING via build-by-backing: every column's
// storage is allocated from the response's arena (SearchResponse::mr, a
// thread-safe pmr view over the request arena), and the concrete column's span
// members point at it.  Single-valued columns are pre-sized in the dispatching
// thread (allocArray(columnSize)) and the parallel per-segment tasks fill
// distinct slots by index.  Multi-valued sub-arrays and copied string values
// are allocated inside the tasks (concurrently) - safe because the arena is
// thread-safe.  String values are copied into the arena (arenaStr) since their
// source views (TermsEnum / column / stored-field readers) are transient.

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <ranges>
#include <span>
#include <variant>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/Collector.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/StrRef.h"
#include "luxir/util/Signal.h"
#include "luxir/schema/FieldPattern.h"
#include "luxir/util/luxir_util.h"
#include "luxir/util/thread.h"

namespace luxir {

// Batch-local row index and segment run length. One batch holds at most
// INT32_MAX rows (the DocList row_count is an int32); the same width serves
// the segment-sorted permutation and the per-segment run lengths.
using RowIndex = uint32_t;
using SegmentRuns = std::span<const std::span<RowIndex>>;
using FieldLoaders = std::vector<std::function<void(std::span<RowIndex>)>>;

template <typename F>
void addFieldLoader(FieldLoaders* loaders, SegmentRuns runs, F body) {
  if (loaders) loaders->emplace_back(std::move(body));
  else for (auto run : runs) body(run);
}

inline void loadFields(oneapi::tbb::task_group* tg, FieldLoaders& loaders, SegmentRuns runs) {
  task_group_fanout(tg, loaders.size() * runs.size(), [&](size_t i) {
    size_t loader = i / runs.size();
    auto run = runs[i % runs.size()];
    return [&loaders, loader, run]() { loaders[loader](run); };
  });
}


// How an op's ranked list reaches the client. STREAM: transport batches of
// bounded size, every batch but the last sent as it completes (the emitter
// may pause on backpressure and outlive the request body). FINAL_RESPONSE:
// one batch holding every row, assembled into the request's final response.
// Required beneath a facet bucket slot: an intermediate response there would
// carry only a skeletal facet path, and a bucket binding does not outlive its
// bucket, so nothing may pause.
enum class DocEmission : uint8_t {
  STREAM,
  FINAL_RESPONSE
};

// Exact missing_val support for single-valued columns.  The sentinel
// contract (v[i] != missing_val) is only 100% if the filler provably does
// not occur as a real value in the batch, so the filler is chosen per
// column per batch: per-segment load tasks mark the slots they fill, and
// after the batch's task-group wait a finish pass picks a filler absent
// from the loaded values, writes it into the unfilled slots, and sets
// missing_val.  Multi-valued columns signal missing structurally (empty
// array) and are not tracked.
// Tasks write distinct bytes of present, so no synchronization is needed
// beyond the task-group wait; the deque keeps element addresses stable
// while tasks hold present.data().
struct PendingCol {
  std::vector<uint8_t> present;
  std::function<void(std::span<const uint8_t>)> finish;
};
using PendingCols = std::deque<PendingCol>;

// Run the finish passes.  Call after the batch's load tasks complete.
inline void finishPendingCols(PendingCols& pendingCols) {
  for (auto& p : pendingCols) {
    p.finish(p.present);
  }
}

// Pick a filler value that does not occur among the present slot values,
// working in the Emit's encoded (order-preserving int64) space.  Preference
// order: 0 (cheapest varint for ints), then the low and high extremes, then
// any unused value.  Emits with foldZeros (floats/doubles) treat +0.0 and
// -0.0 as one value, since clients compare with ==; NaN is never chosen (it
// lies outside [encLo, encHi]).  No sorting: the common path is a single
// scan testing three candidates, and the rare fallback uses pigeonhole.
template <typename Emit>
typename Emit::value_type pickMissingVal(std::span<const typename Emit::value_type> vals,
                                         std::span<const uint8_t> present) {
  using V = typename Emit::value_type;
  const int64_t encLo = Emit::encLo();
  const int64_t encHi = Emit::encHi();

  // One scan testing only the three preferred fillers.  encodeVal((V)0) is 0
  // for every type, so the zero test is e == 0; for floats a present -0.0
  // (which encodes to -1) also blocks the +0.0 filler since they compare ==.
  bool zeroTaken = false, loTaken = false, hiTaken = false;
  for (size_t i = 0; i < vals.size(); i++) {
    if (!present[i]) continue;
    int64_t e = Emit::encodeVal(vals[i]);
    if (e == 0 || (Emit::foldZeros && e == -1)) zeroTaken = true;
    if (e == encLo) loTaken = true;
    if (e == encHi) hiTaken = true;
  }
  if (!zeroTaken) return (V)0;
  if (!loTaken) return Emit::decodeEnc(encLo);
  if (!hiTaken) return Emit::decodeEnc(encHi);

  // Degenerate batch: it holds 0 and both extremes.  Among the n+1
  // consecutive encodings [encLo, encLo+n] (n = present count) at most n can
  // be present, so one is free.  Mark the in-window values and return the
  // first hole - O(n), no sort.
  int64_t n = 0;
  for (size_t i = 0; i < present.size(); i++) n += present[i] ? 1 : 0;
  std::vector<char> taken(n + 1, 0);
  for (size_t i = 0; i < vals.size(); i++) {
    if (!present[i]) continue;
    // Unsigned: for the int column encLo is INT64_MIN, so encodeVal - encLo
    // overflows signed int64 (UB, miscompiled at -O2) for any value above
    // encLo.  In unsigned arithmetic the window [encLo, encLo+n] maps to
    // off in [0, n]; everything outside wraps to a large value that the
    // single off <= n test rejects.
    uint64_t off = (uint64_t)Emit::encodeVal(vals[i]) - (uint64_t)encLo;
    if (off <= (uint64_t)n) taken[off] = 1;
  }
  for (int64_t j = 0; j <= n; j++) {
    if (!taken[j]) return Emit::decodeEnc(encLo + j);
  }
  return Emit::decodeEnc(encLo);  // unreachable by pigeonhole
}

// One stored-field retrieval request.  Collected per-field during dispatch
// then grouped by resource so each (resource, segment) pair is served by a
// single StoredFieldsReader - one chunk decompression covers all fields
// sharing the resource.
struct StoredReq {
  std::string_view fieldName;
  FieldType* fieldType;
  IndexReader::FieldSegments infos;
  // Exactly one of starget / mtarget is populated (selected by multi flag).
  bool multi;
  // Mutable views over the arena-allocated column slots (single: one
  // string_view per doc; multi: one ArrStr per doc).  Filled by the loaders.
  std::span<std::string_view> starget;
  std::span<luxir::api::ArrStr> mtarget;
  uint8_t* present = nullptr;  // presence slots when single-valued (see PendingCol)
};

// Allocate the output Column (col_s or multi_s) for a TEXT/STRING field and
// return mutable spans into its arena storage.  Used by both column retrieval
// (loadStrCol) and stored retrieval - the output shape is identical.  Leaves
// spans empty when columnSize is 0.  Returns the presence slots for a
// single-valued column (null for multi, which signals missing structurally).
inline uint8_t* allocStringColumn(luxir::api::Column& fieldCol, size_t columnSize, bool multi,
                                  std::span<std::string_view>& starget,
                                  std::span<luxir::api::ArrStr>& mtarget,
                                  PendingCols& pendingCols, std::pmr::memory_resource& mr)
{
  if (columnSize == 0) return nullptr;
  if (!multi) {
    auto& strCol = fieldCol.kind.emplace<luxir::api::ColStr>();
    starget = std::span<std::string_view>(build::allocArray(strCol.v, columnSize, mr), columnSize);

    auto& pending = pendingCols.emplace_back();
    pending.present.assign(columnSize, 0);
    pending.finish = [&strCol, &mr](std::span<const uint8_t> present) {
      auto v = std::span<std::string_view>(const_cast<std::string_view*>(strCol.v.data()), strCol.v.size());
      // "" is the default filler; only when a real empty string occurs must
      // a different filler be used: one byte past the largest present value
      // is greater than every present value, so it cannot collide.
      bool emptyPresent = false;
      for (size_t i = 0; i < present.size(); i++) {
        if (present[i] && v[i].empty()) {
          emptyPresent = true;
          break;
        }
      }
      if (!emptyPresent) return;
      std::string_view maxStr;
      for (size_t i = 0; i < present.size(); i++) {
        if (present[i] && v[i] > maxStr) maxStr = v[i];
      }
      std::string filler = std::string(maxStr) + '\0';
      strCol.missing_val = build::arenaStr(mr, filler);  // copy filler into the arena
      for (size_t i = 0; i < present.size(); i++) {
        if (!present[i]) v[i] = strCol.missing_val;
      }
    };
    return pending.present.data();
  } else {
    auto& strCol = fieldCol.kind.emplace<luxir::api::ArrArrStr>();
    mtarget = std::span<luxir::api::ArrStr>(build::allocArray(strCol.v, columnSize, mr), columnSize);
    return nullptr;
  }
}

// Per-segment body of loadStrCol.  Reads the field's column for one
// segment and writes values into starget (single) or mtarget (multi).
// No-op if the field isn't in the segment.  Extracted so the stored-fields
// path can fall back here when a segment predates the STORED flag.
inline void loadStrColForSegment(SearchRequest& req, const SegFieldInfo* info, FieldType& fieldType,
                                 std::span<RowIndex> idxSpan, std::span<const segdoc> segDocs,
                                 std::span<std::string_view> starget, std::span<luxir::api::ArrStr> mtarget,
                                 uint8_t* present, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  if (!info) return;
  const auto& segFieldInfo = *info;

  bool isIndexedString = (segFieldInfo.flags & FieldType::INDEX_DOCS) != 0;

  if (!fieldType.multiValued()) {
    if (isIndexedString) {
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      auto valHandler = [&](size_t idx, int32_t doc, int32_t val) {
        assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
        tenum.seekOrd((int32_t) val - 1);
        starget[idxSpan[idx]] = build::arenaStr(mr, (std::string_view) tenum.term());
        if (present) present[idxSpan[idx]] = 1;
      };
      OrdColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    } else {
      auto valHandler = [&](size_t idx, int32_t doc, std::string_view val) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        starget[idxSpan[idx]] = build::arenaStr(mr, val);
        if (present) present[idxSpan[idx]] = 1;
      };
      StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    }
  } else {
    if (isIndexedString) {
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      auto valHandler = [&](size_t idx, int32_t doc, int32_t val, int64_t valIdx, int64_t numVals) {
        assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
        luxir::api::ArrStr& target = mtarget[idxSpan[idx]];
        if (valIdx == 0) build::allocArray(target.v, numVals, mr);
        tenum.seekOrd((int32_t) val - 1);
        const_cast<std::string_view*>(target.v.data())[valIdx] = build::arenaStr(mr, (std::string_view) tenum.term());
      };
      OrdColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    } else {
      auto valHandler = [&](size_t idx, int32_t doc, std::string_view val, int64_t valIdx, int64_t numVals) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        luxir::api::ArrStr& target = mtarget[idxSpan[idx]];
        if (valIdx == 0) build::allocArray(target.v, numVals, mr);
        const_cast<std::string_view*>(target.v.data())[valIdx] = build::arenaStr(mr, val);
      };
      StrColReader::getMultiValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    }
  }
}

// loadStrCol when the output spans are already allocated (by the caller's
// earlier allocStringColumn).  Dispatches one per-segment task per run.
inline void loadStrColWithTargets(SearchRequest& req, IndexReader::FieldSegments infos, FieldType& fieldType,
                                  std::span<std::string_view> starget, std::span<luxir::api::ArrStr> mtarget,
                                  uint8_t* present,
                                  std::span<const segdoc> segDocs, SegmentRuns runs, FieldLoaders* loaders,
                                  std::pmr::memory_resource& mr)
{
  addFieldLoader(loaders, runs, [&req, infos, &fieldType, segDocs, starget, mtarget, present, &mr](std::span<RowIndex> idxSpan) {
    loadStrColForSegment(req, infos.at(segDocs[idxSpan[0]].segment()), fieldType, idxSpan, segDocs, starget, mtarget, present, mr);
  });
}

inline void loadStrCol(SearchRequest& req, IndexReader::FieldSegments infos, FieldType& fieldType,
                       std::span<const segdoc> segDocs, SegmentRuns runs,
                       luxir::api::Column& fieldCol,
                       FieldLoaders* loaders, PendingCols& pendingCols, std::pmr::memory_resource& mr)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;
  std::span<std::string_view> starget;
  std::span<luxir::api::ArrStr> mtarget;
  uint8_t* present = allocStringColumn(fieldCol, columnSize, fieldType.multiValued(),
                                       starget, mtarget, pendingCols, mr);
  loadStrColWithTargets(req, infos, fieldType, starget, mtarget, present, segDocs, runs, loaders, mr);
}

// Per-type policies for emitting numeric columns.  INT columns hold the
// value directly; FLOAT/DOUBLE columns hold sortable bits (see
// util/NumericUtils.h) that decode() turns back into the client-facing
// floating point value.  encodeVal/decodeEnc map client-facing values to
// the order-preserving int64 space pickMissingVal searches in (identity
// for ints, sortable bits for floats/doubles).
struct IntColEmit {
  using value_type = int64_t;
  using arr_type = luxir::api::ArrInt;
  static constexpr bool foldZeros = false;
  static auto& singleCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ColInt>(); }
  static auto& multiCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ArrArrInt>(); }
  static int64_t decode(int64_t raw) { return raw; }
  static int64_t encodeVal(int64_t v) { return v; }
  static int64_t decodeEnc(int64_t e) { return e; }
  static int64_t encLo() { return std::numeric_limits<int64_t>::min(); }
  static int64_t encHi() { return std::numeric_limits<int64_t>::max(); }
};

struct FloatColEmit {
  using value_type = float;
  using arr_type = luxir::api::ArrFloat;
  static constexpr bool foldZeros = true;
  static auto& singleCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ColFloat>(); }
  static auto& multiCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ArrArrFloat>(); }
  static float decode(int64_t raw) { return sortableInt32ToFloat((int32_t)raw); }
  static int64_t encodeVal(float v) { return (int64_t)floatToSortableInt32(v); }
  static float decodeEnc(int64_t e) { return sortableInt32ToFloat((int32_t)e); }
  static int64_t encLo() { return (int64_t)floatToSortableInt32(-std::numeric_limits<float>::infinity()); }
  static int64_t encHi() { return (int64_t)floatToSortableInt32(std::numeric_limits<float>::infinity()); }
};

struct DoubleColEmit {
  using value_type = double;
  using arr_type = luxir::api::ArrDouble;
  static constexpr bool foldZeros = true;
  static auto& singleCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ColDouble>(); }
  static auto& multiCol(luxir::api::Column& col) { return col.kind.emplace<luxir::api::ArrArrDouble>(); }
  static double decode(int64_t raw) { return sortableInt64ToDouble(raw); }
  static int64_t encodeVal(double v) { return doubleToSortableInt64(v); }
  static double decodeEnc(int64_t e) { return sortableInt64ToDouble(e); }
  static int64_t encLo() { return doubleToSortableInt64(-std::numeric_limits<double>::infinity()); }
  static int64_t encHi() { return doubleToSortableInt64(std::numeric_limits<double>::infinity()); }
};

// The caller finishes the batch and waits before releasing loader resources.
template <typename Emit>
inline void loadNumCol(SearchRequest& req, IndexReader::FieldSegments infos, FieldType& fieldType,
                       std::span<const segdoc> segDocs, SegmentRuns runs,
                       luxir::api::Column& fieldCol,
                       FieldLoaders* loaders, PendingCols& pendingCols, std::pmr::memory_resource& mr)
{
  std::span<typename Emit::value_type> starget; // single valued target
  std::span<typename Emit::arr_type> mtarget;   // multi-valued target
  uint8_t* present = nullptr;
  auto columnSize = segDocs.size();

  if (columnSize == 0) {
    return;
  }

  if (!fieldType.multiValued()) {
    auto& numCol = Emit::singleCol(fieldCol);
    starget = std::span<typename Emit::value_type>(build::allocArray(numCol.v, columnSize, mr), columnSize);

    auto& pending = pendingCols.emplace_back();
    pending.present.assign(columnSize, 0);
    present = pending.present.data();
    pending.finish = [&numCol](std::span<const uint8_t> present) {
      auto* data = const_cast<typename Emit::value_type*>(numCol.v.data());
      size_t n = present.size();
      auto filler = pickMissingVal<Emit>({data, n}, present);
      numCol.missing_val = filler;
      for (size_t i = 0; i < n; i++) {
        if (!present[i]) data[i] = filler;
      }
    };
  } else {
    auto& numCol = Emit::multiCol(fieldCol);
    mtarget = std::span<typename Emit::arr_type>(build::allocArray(numCol.v, columnSize, mr), columnSize);
  }
  addFieldLoader(loaders, runs, [&req, infos, &fieldType, segDocs, starget, mtarget, present, &mr](std::span<RowIndex> idxSpan) {
    auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
    auto segNum = sortedSegDocs[0].segment();
    auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

    auto& postingsReader = req.reader->segments()[segNum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto* info = infos.at(segNum);
    if (!info) return;
    const auto& segFieldInfo = *info;

    if (!fieldType.multiValued()) {
      auto valHandler = [&](size_t idx, int32_t doc, int64_t val) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        starget[idxSpan[idx]] = Emit::decode(val);
        present[idxSpan[idx]] = 1;
      };
      IntColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    } else {
      auto valHandler = [&](size_t idx, int32_t doc, int64_t val, int64_t valIdx, int64_t numVals) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        typename Emit::arr_type& target = mtarget[idxSpan[idx]];
        if (valIdx == 0) {
          build::allocArray(target.v, numVals, mr);
        }
        const_cast<typename Emit::value_type*>(target.v.data())[valIdx] = Emit::decode(val);
      };
      IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    }
  });
}

inline void loadVersionRuns(SearchRequest& req, std::span<const segdoc> segDocs, SegmentRuns runs,
                            luxir::api::Column& fieldCol, FieldLoaders* loaders,
                            PendingCols& pendingCols, std::pmr::memory_resource& mr) {
  auto& col = fieldCol.kind.emplace<luxir::api::ColInt>();
  auto* target = build::allocArray(col.v, segDocs.size(), mr);
  auto& pending = pendingCols.emplace_back();
  pending.present.assign(segDocs.size(), 1);
  pending.finish = [&col](std::span<const uint8_t> present) {
    col.missing_val = pickMissingVal<IntColEmit>(col.v, present);
  };
  addFieldLoader(loaders, runs, [&req, segDocs, target](std::span<RowIndex> indices) {
    auto segment = segDocs[indices[0]].segment();
    VersionRuns::Cursor cursor(req.reader->segments()[segment].postingsReader().versionRuns());
    for (auto index : indices) target[index] = (int64_t)cursor.birth(segDocs[index].docId());
  });
}

// Copy raw column bytes into a Vector.f32's arena float buffer.  memcpy
// sidesteps alignment of the source bytes (column storage is byte-packed
// and not guaranteed 4-byte aligned).
inline void fillVectorF32(luxir::api::Vector& vec, std::string_view bytes, std::pmr::memory_resource& mr) {
  assert(bytes.size() % sizeof(float) == 0);
  int32_t nFloats = (int32_t)(bytes.size() / sizeof(float));
  vec.f32.emplace();
  float* fs = build::allocArray(vec.f32->v, nFloats, mr);
  std::memcpy(fs, bytes.data(), bytes.size());
}

inline void loadVectorColForSegmentSingle(SearchRequest& req, const SegFieldInfo* info,
                                          std::span<RowIndex> idxSpan,
                                          std::span<const segdoc> segDocs,
                                          std::span<luxir::api::Vector> target, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  if (!info) return;
  const auto& segFieldInfo = *info;

  auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes) {
    assert(segDocs[idxSpan[idx]].docId() == doc);
    unused(doc);
    fillVectorF32(target[idxSpan[idx]], bytes, mr);
  };
  StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
}

inline void loadVectorColForSegmentMulti(SearchRequest& req, const SegFieldInfo* info,
                                         std::span<RowIndex> idxSpan,
                                         std::span<const segdoc> segDocs,
                                         std::span<luxir::api::ArrVector> target, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  if (!info) return;
  const auto& segFieldInfo = *info;

  // Multi-valued vectors: each doc's ArrVector holds numVals Vectors. numVals
  // is known at valIdx==0, so the inner Vector span is allocArray'd then filled
  // by index (recovered via the span's data() across valHandler calls).
  auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes, int64_t valIdx, int64_t numVals) {
    assert(segDocs[idxSpan[idx]].docId() == doc);
    unused(doc);
    luxir::api::ArrVector& outer = target[idxSpan[idx]];
    if (valIdx == 0) build::allocArray(outer.v, numVals, mr);
    fillVectorF32(const_cast<luxir::api::Vector*>(outer.v.data())[valIdx], bytes, mr);
  };
  StrColReader::getMultiValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
}

// Decode a VECTOR column's bytes back to floats in the response.
//
// Single-valued output: Column.col_vec - one Vector per doc; missing docs
// leave the slot's kind oneof unset.
// Multi-valued output:  Column.multi_vec - one ArrVector per doc; missing
// docs (or docs that indexed an empty list) leave an empty ArrVector.
inline void loadVectorCol(SearchRequest& req, IndexReader::FieldSegments infos, FieldType& fieldType,
                          std::span<const segdoc> segDocs, SegmentRuns runs,
                          luxir::api::Column& fieldCol,
                          FieldLoaders* loaders, std::pmr::memory_resource& mr)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;

  if (!fieldType.multiValued()) {
    auto& vecCol = fieldCol.kind.emplace<luxir::api::ColVector>();
    std::span<luxir::api::Vector> target(build::allocArray(vecCol.v, columnSize, mr), columnSize);

    addFieldLoader(loaders, runs, [&req, infos, segDocs, target, &mr](std::span<RowIndex> idxSpan) {
      loadVectorColForSegmentSingle(req, infos.at(segDocs[idxSpan[0]].segment()), idxSpan, segDocs, target, mr);
    });
  } else {
    auto& arrVecCol = fieldCol.kind.emplace<luxir::api::MultiVector>();
    std::span<luxir::api::ArrVector> target(build::allocArray(arrVecCol.v, columnSize, mr), columnSize);

    addFieldLoader(loaders, runs, [&req, infos, segDocs, target, &mr](std::span<RowIndex> idxSpan) {
      loadVectorColForSegmentMulti(req, infos.at(segDocs[idxSpan[0]].segment()), idxSpan, segDocs, target, mr);
    });
  }
}

// Copy stored values for one doc into a request's output slot.
inline void storeValuesInReq(const StoredReq& r, size_t slot, std::span<const std::string_view> values,
                             std::pmr::memory_resource& mr) {
  if (values.empty()) return;
  if (!r.multi) {
    r.starget[slot] = build::arenaStr(mr, values[0]);
    if (r.present) r.present[slot] = 1;
  } else {
    luxir::api::ArrStr& target = r.mtarget[slot];
    std::string_view* dst = build::allocArray(target.v, values.size(), mr);
    for (size_t i = 0; i < values.size(); i++) {
      dst[i] = build::arenaStr(mr, values[i]);
    }
  }
}

// Dispatch stored-fields retrieval for all fields that share one
// resource.  Each segment run launches a single task that opens one
// StoredFieldsReader and decodes every field in the group from the
// shared decompressed chunk.
//
// Per-segment the group is partitioned into (a) fields that exist in the
// segment's stored-fields resource - served from the chunk - and
// (b) fields that don't - served from their column as a fallback, or
// left empty if the field also lacks a column.  This handles both
// pre-STORED segments (no SFR at all) and partial-stored segments (SFR
// present but missing a specific field added later).
inline void loadStoredFields(SearchRequest& req, IndexReader::FieldSegments infos,
                             std::vector<StoredReq> reqsOwned,
                             std::span<const segdoc> segDocs, SegmentRuns runs, FieldLoaders* loaders,
                             std::pmr::memory_resource& mr)
{
  addFieldLoader(loaders, runs, [&req, infos, reqs = std::move(reqsOwned), segDocs, &mr](std::span<RowIndex> idxSpan) {
    auto segNum = segDocs[idxSpan[0]].segment();
    auto& postingsReader = req.reader->segments()[segNum].postingsReader();
    auto* info = infos.at(segNum);
    auto sfr = info ? std::make_unique<StoredFieldsReader>(postingsReader, *info) : nullptr;

    // Partition reqs: sfrReqs serve from SFR, colReqs fall back to
    // column.  The fallback is attempted for any STRING/ID field type
    // (not just fields whose current schema sets hasColumn): older
    // segments predating a schema change may still have the ord/value
    // column on disk, and loadStrColForSegment no-ops gracefully if the
    // field is actually absent.  TEXT fields have no retrievable column
    // - they stay empty when missing from the segment's SFR.
    std::vector<const StoredReq*> sfrReqs;
    std::vector<const StoredReq*> colReqs;
    sfrReqs.reserve(reqs.size());
    for (const auto& r : reqs) {
      if (sfr && sfr->hasField(r.fieldName)) {
        sfrReqs.push_back(&r);
        continue;
      }
      auto t = r.fieldType->type();
      if (t == FieldType::Type::STRING || t == FieldType::Type::ID) {
        colReqs.push_back(&r);
      }
    }

    for (auto* r : colReqs) {
      loadStrColForSegment(req, r->infos.at(segNum), *r->fieldType, idxSpan, segDocs, r->starget, r->mtarget,
                           r->present, mr);
    }

    if (sfrReqs.empty()) return;

    if (sfrReqs.size() == 1) {
      // Single-field fast path: readFieldById skips other stored
      // fields in each doc without materializing their value views.
      // Resolve fid once per segment rather than on every doc call.
      auto* r = sfrReqs[0];
      int32_t fid = sfr->fieldId(r->fieldName);
      assert(fid >= 0);  // partitioning above guarantees presence
      for (int32_t i = 0; i < (int32_t)idxSpan.size(); i++) {
        int32_t doc = segDocs[idxSpan[i]].docId();
        sfr->readFieldById(doc, fid, [&](std::span<const std::string_view> values) {
          storeValuesInReq(*r, idxSpan[i], values, mr);
        });
      }
    } else {
      // Multi-field path: one readDoc pass per doc, matching each
      // entry against the sfr-backed request list.  Typical size is
      // small so a linear scan is fine.
      for (int32_t i = 0; i < (int32_t)idxSpan.size(); i++) {
        int32_t doc = segDocs[idxSpan[i]].docId();
        sfr->readDoc(doc, [&](std::string_view name, std::span<const std::string_view> values) {
          for (auto* r : sfrReqs) {
            if (r->fieldName == name) {
              storeValuesInReq(*r, idxSpan[i], values, mr);
            }
          }
        });
      }
    }
  });
}

// Row placement: scatter columns into per-doc field maps (DocList.docs).
// The loaders always fill column-shaped storage (pre-sized spans, filled in
// parallel per segment); placeBatchColumns takes the columns that belong in
// rows out of the response, and this pass moves their values into rows.
// Presence follows the column contracts (see forEachPresent).  Missing = key
// absent; no sentinels in rows.  Values share the arena-backed spans and
// string views with the moved columns - nothing is copied.
//
// Returns the mutable row array so the caller can add pseudo-fields
// (_score_).  rowCaps[i] is row i's exact map capacity: its present values
// plus the pseudo-fields the caller will add.
inline luxir::api::Map* scatterColumnsToRows(const SearchResponse::ColumnsType& cols,
                                             luxir::api::DocList& docListProto,
                                             size_t numDocs, std::span<const uint32_t> rowCaps,
                                             std::pmr::memory_resource& mr)
{
  using luxir::api::Val;
  luxir::api::Map* rows = build::allocArray(docListProto.docs, numDocs, mr);
  for (const auto& [field, col] : cols) {
    auto slot = [&](size_t i) -> Val& {
      return *build::mapSlot<Val>(rows[i].fields, rowCaps[i], field, mr);
    };
    if (auto* c = std::get_if<luxir::api::ColStr>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i] != c->missing_val) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ColInt>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i] != c->missing_val) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ColFloat>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i] != c->missing_val) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ColDouble>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i] != c->missing_val) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ArrArrStr>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ArrArrInt>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ArrArrFloat>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ArrArrDouble>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ColVal>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].isNull()) slot(i) = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::ColVector>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i].f32.has_value()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::MultiVector>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    }
    // monostate can't occur here (loaders always emplace a kind when
    // numDocs > 0).
  }
  return rows;
}

// One resolved returned-field entry (see resolveReturnFields). The output key
// retains the request spelling; physicalName + fieldType identify the source.
// fieldType is resolved from the request schema at selector resolution, so explicit-name
// errors surface before any batch is assembled and the emit loop does no
// per-batch schema lookups; kind selects the physical, score, or version source.
// discovered: from an empty or wildcard selector (COLUMNS leaves such a column
// out of a batch where it has no value).  infos and storedInfos are bound with the
// first rows (see resolveReturnFieldMetadata), so a count-only response never
// looks them up.
struct ReturnField {
  enum class Kind { PHYSICAL, SCORE, VERSION };

  std::string_view outputKey;
  std::string_view physicalName;
  FieldType* fieldType;
  Kind kind;
  bool discovered;
  IndexReader::FieldSegments infos;
  IndexReader::FieldSegments storedInfos;
};

inline void resolveReturnFieldMetadata(IndexReader& reader, std::span<ReturnField> fields) {
  boost::unordered_flat_map<std::string_view, IndexReader::FieldSegments, PackedTermHash, PackedTermEqual> resources;
  for (auto& rf : fields) {
    if (rf.kind != ReturnField::Kind::PHYSICAL) continue;
    if (rf.fieldType->type() != FieldType::TEXT) rf.infos = reader.field(rf.physicalName);
    if (rf.fieldType->isStored()) {
      auto [it, inserted] = resources.try_emplace(rf.fieldType->storedResource_);
      if (inserted) it->second = reader.field(it->first);
      rf.storedInfos = it->second;
    }
  }
}

// Calls f(i) for each slot i of col that holds a value: single-valued
// columns by missing_val (exact after finishPendingCols), multi-valued ones
// by a non-empty array, vector and value columns by a set oneof.  present,
// when non-null, is the column's PendingCol slot tracker (1 = filled), which
// finishPendingCols makes equivalent; scanning it skips the value compares.
template <typename F>
inline void forEachPresent(const luxir::api::Column& col, const uint8_t* present, F&& f) {
  using namespace luxir::api;
  std::visit([&](const auto& c) {
    using T = std::decay_t<decltype(c)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return;
    } else {
      for (size_t i = 0; i < c.v.size(); i++) {
        bool has;
        if constexpr (std::is_same_v<T, ColStr> || std::is_same_v<T, ColInt>
                      || std::is_same_v<T, ColFloat> || std::is_same_v<T, ColDouble>) {
          has = present != nullptr ? present[i] != 0 : c.v[i] != c.missing_val;
        } else if constexpr (std::is_same_v<T, ColVal>) {
          has = !c.v[i].isNull();
        } else if constexpr (std::is_same_v<T, ColVector>) {
          has = c.v[i].f32.has_value();
        } else {
          has = !c.v[i].v.empty();
        }
        if (has) f(i);
      }
    }
  }, col.kind);
}

// Wire size of v as a protobuf varint.
inline size_t varintSize(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

// The wire bytes one missing slot of col costs: zigzag varint of missing_val
// for packed sint64, 4/8 for packed float/double, tag + varint(len) + len of
// missing_val for repeated string, and tag + zero length for an element
// message (multi-valued, vector and value columns).
inline size_t missingSlotBytes(const luxir::api::Column& col) {
  using namespace luxir::api;
  return std::visit([](const auto& c) -> size_t {
    using T = std::decay_t<decltype(c)>;
    if constexpr (std::is_same_v<T, ColStr>) {
      return 1 + varintSize(c.missing_val.size()) + c.missing_val.size();
    } else if constexpr (std::is_same_v<T, ColInt>) {
      int64_t m = (int64_t)c.missing_val;
      return varintSize(((uint64_t)m << 1) ^ (uint64_t)(m >> 63));
    } else if constexpr (std::is_same_v<T, ColFloat>) {
      return 4;
    } else if constexpr (std::is_same_v<T, ColDouble>) {
      return 8;
    } else {
      return 2;
    }
  }, col.kind);
}

// Settle one batch's placement after finishPendingCols.  columnFields[i] is
// the ReturnField behind response column i and columnPresent[i] its slot
// tracker (null when untracked, see forEachPresent); later columns (_score_)
// always stay.  ROWS moves every column to docs.  COLUMNS leaves out a
// discovered column with no present value; a named one is always present.
// ADAPTIVE leaves out every column with no present value and moves a column
// to docs when its missing slots cost more than per-document entries for its
// present values would ((n - p) * missingSlotBytes > p * (key + 4)).  The
// response column map is compacted in place and the moved columns land in
// moved (exact capacity) for scatterColumnsToRows.  rowCaps receives each
// row's map capacity: its moved values plus one for a row-placed _score_
// (scoreRows); it is left empty when no row map is needed.  fate is
// caller-owned scratch reused across batches.
inline void placeBatchColumns(luxir::api::DocFormat placement,
                              std::span<const ReturnField* const> columnFields,
                              std::span<const uint8_t* const> columnPresent,
                              SearchResponse::ColumnsType& columns,
                              SearchResponse::ColumnsType& moved,
                              size_t numDocs, bool scoreRows, std::vector<uint8_t>& fate,
                              std::vector<uint32_t>& rowCaps,
                              std::pmr::memory_resource& mr)
{
  enum : uint8_t { KEEP, DROP, MOVE };
  using Pair = std::pair<std::string_view, luxir::api::Column>;
  auto* arr = const_cast<Pair*>(columns.data());
  size_t count = std::min(columns.size(), columnFields.size());
  fate.assign(columns.size(), KEEP);
  size_t changed = 0, numMoved = 0;
  for (size_t i = 0; i < count; i++) {
    const ReturnField& rf = *columnFields[i];
    if (placement == luxir::api::DocFormat::ROWS) {
      fate[i] = MOVE;
    } else if (placement == luxir::api::DocFormat::ADAPTIVE || rf.discovered) {
      size_t p = 0;
      forEachPresent(arr[i].second, columnPresent[i], [&p](size_t) { p++; });
      if (p == 0) {
        fate[i] = DROP;
      } else if (placement == luxir::api::DocFormat::ADAPTIVE
                 && (numDocs - p) * missingSlotBytes(arr[i].second) > p * (rf.outputKey.size() + 4)) {
        fate[i] = MOVE;
      }
    }
    if (fate[i] != KEEP) changed++;
    if (fate[i] == MOVE) numMoved++;
  }
  rowCaps.clear();
  if (numMoved > 0 || scoreRows) rowCaps.assign(numDocs, scoreRows ? 1 : 0);
  if (changed == 0) return;
  Pair* out = nullptr;
  if (numMoved > 0) {
    out = (Pair*)mr.allocate(sizeof(Pair) * numMoved, alignof(Pair));
    std::uninitialized_value_construct_n(out, numMoved);
  }
  size_t kept = 0, m = 0;
  for (size_t i = 0; i < columns.size(); i++) {
    if (fate[i] == MOVE) {
      forEachPresent(arr[i].second, columnPresent[i], [&rowCaps](size_t row) { rowCaps[row]++; });
      out[m++] = std::move(arr[i]);
    } else if (fate[i] == KEEP) {
      if (kept != i) arr[kept] = std::move(arr[i]);
      kept++;
    }
  }
  columns = SearchResponse::ColumnsType(std::span<const Pair>(arr, kept));
  moved = SearchResponse::ColumnsType(std::span<const Pair>(out, numMoved));
}

// Streams a final ranked list of (segdoc, score) pairs back to the client as
// one or more SearchResponses.  Both TopDocsReq and FusionOp use this (via
// emitDocsResponse below) after their own ranking is complete.  The caller's
// `getDocList(response)` callback returns the DocList in `response` (a
// SearchResponse*) to populate.
//
// `getDoc(i)` and `getScore(i)` are invoked lazily, only for indices in the
// current batch - no upfront materialization of a parallel array, so callers
// can wire them directly to whatever layout they have (collector ScoreDoc /
// SortDoc spans, RRF result vectors, etc.).  `getScore` is only called when
// `getScores` is true.
//
// Emission is resumable: each non-final batch goes out via req.reply(), and a
// PAUSE status (connection over its buffer high-water mark) parks the emitter
// until the transport drains it (SearchRequest::resumeWhenDrained).  The
// emitter can therefore outlive submitBody(): it is allocated in the request
// arena, and everything its callbacks reference is pinned by req.rootCalc
// (calculator tree: collector output, getTarget chain) or owned by the
// callbacks themselves.  That pin covers STREAM emitters only; a
// FINAL_RESPONSE emitter (a facet bucket child) runs to completion inside
// produce() and its callbacks are never used again, so its calculator may be
// destroyed with the bucket.  The final batch is assembled into req.lastResponse
// but NOT sent here - the completion protocol on SearchRequest (streamEnded /
// bodyDone) decides who sends it.  All but the final response have more=true.
using DocBatchMetadata = std::function<void(luxir::api::DocList&, int64_t, int64_t,
                                           std::pmr::memory_resource&)>;

class DocEmitter {
public:
  virtual void produce() = 0;
  virtual ~DocEmitter() = default;
};

template <typename GetDocList, typename GetDoc, typename GetScore>
class DocEmitterImpl final : public DocEmitter {
public:
  SearchRequest& req;
  GetDocList getDocList;
  GetDoc getDoc;
  GetScore getScore;
  int64_t numCollected;
  int64_t totalHits;
  DocBatchMetadata metadata;
  std::span<ReturnField> fields;  // resolved selectors
  int32_t maxBatchSize;
  int64_t offset;
  bool emitCount;
  bool foundGte;
  bool getScores;
  // Resolved DocFormat: ROWS, COLUMNS or ADAPTIVE.  Every field loads as a
  // column; placeBatchColumns settles each batch's placement.  The synthetic
  // _score_ is written per row under ROWS and is a column otherwise.
  luxir::api::DocFormat placement;
  // Snapshot of req.tg != nullptr: req.tg points at submit()'s stack and may
  // dangle by the time a resumed emitter runs.
  bool parallel;
  size_t colCap;  // response columns map capacity (fields + _score_)
  int64_t batchStart;  // absolute rank cursor, clamped to numCollected
  bool fieldMetadataResolved = false;
  // Per-batch placement scratch, reused across batches: columnFields maps
  // response columns to their ReturnField, columnPresent to their PendingCol
  // slot tracker (or null); columnFate and rowCaps belong to
  // placeBatchColumns.
  std::vector<const ReturnField*> columnFields;
  std::vector<const uint8_t*> columnPresent;
  std::vector<uint8_t> columnFate;
  std::vector<uint32_t> rowCaps;

  DocEmitterImpl(SearchRequest& req, GetDocList getDocList, GetDoc getDoc,
                 GetScore getScore, int64_t numCollected, int64_t totalHits,
                 std::span<ReturnField> fields, int32_t maxBatchSize,
                 int64_t offset, bool emitCount, bool foundGte, bool getScores,
                 luxir::api::DocFormat placement, bool parallel, size_t colCap,
                 DocBatchMetadata metadata)
      : req(req), getDocList(std::move(getDocList)), getDoc(std::move(getDoc)),
        getScore(std::move(getScore)), numCollected(numCollected), totalHits(totalHits),
        metadata(std::move(metadata)), fields(fields), maxBatchSize(maxBatchSize), offset(offset), emitCount(emitCount),
        foundGte(foundGte), getScores(getScores), placement(placement),
        parallel(parallel), colCap(colCap),
        batchStart(std::min(offset, numCollected)) {}

  void produce() override;

private:
  // Returns true when the stream is over (all batches produced, or cancelled);
  // false when paused (a resume callback re-enters produce()).  May throw.
  bool produceBatches();
};

template <typename GetDocList, typename GetDoc, typename GetScore>
void DocEmitterImpl<GetDocList, GetDoc, GetScore>::produce() {
  // Exceptions must not escape: a resumed emitter runs detached on a task-arena
  // thread with no catch frame, and even on the synchronous first call an
  // escaping exception would leave the stream registered and the final
  // response unsendable.  Record the error on the final response instead
  // (mirrors submitBody's catch) and end the stream normally.
  bool finished = true;
  try {
    finished = produceBatches();
  } catch (...) {
    // Emission is execution: an unclassified failure is the engine's.  The
    // request-authored cases (an unknown returned field, an expression that
    // cannot be evaluated) throw ApiErrors and keep their classification.
    req.setError(currentExceptionInfo(ErrorKind::INTERNAL));
  }
  if (finished) {
    // The final batch (assembled into req.lastResponse by produceBatches) is
    // sent by the completion protocol; this may delete the request.
    req.streamEnded();
  }
}

template <typename GetDocList, typename GetDoc, typename GetScore>
bool DocEmitterImpl<GetDocList, GetDoc, GetScore>::produceBatches() {
  // An empty page still produces exactly one final DocList. Resumption only
  // happens after a non-final batch, with ranked documents left to emit.
  do {
    // A request that already failed (a sibling op threw while this stream
    // was paused) emits nothing more; the error goes out in the final response.
    {
      std::lock_guard<std::mutex> lock(req.mutex);
      if (req.lastResponse->proto.error.has_value()) return true;
    }
    int64_t batchEnd = batchStart + std::min<int64_t>(maxBatchSize, numCollected - batchStart);
    int64_t batchSizeLocal = batchEnd - batchStart;

    bool lastResponse = batchEnd >= numCollected;
    auto& response = lastResponse ? *req.lastResponse : *SearchResponse::create(req, lastResponse);
    // A non-final batch has its own arena, released by reply()'s cleanup.  Until
    // reply() takes over, this guard owns it so a throwing field lookup/loader
    // (caught in produce()) does not leak the arena.
    struct BatchArenaGuard {
      google::protobuf::Arena* arena = nullptr;
      ~BatchArenaGuard() { if (arena) releaseArena(arena); }
    } batchArenaGuard;
    if (!lastResponse) batchArenaGuard.arena = &response.arena;
    auto& docListProto = getDocList(&response);
    auto& mr = response.mr;  // arena backing this response's column data
    // Absolute rank of this batch's first row. An empty page (offset past the
    // collected list) echoes the requested offset instead.
    docListProto.offset = std::max(offset, batchStart);
    if (!lastResponse) {
      docListProto.more = true;
      response.proto.more = true;  // also set at the response level for easier client handling.
    }

    if (emitCount) {
      docListProto.found = totalHits;
      docListProto.found_gte = foundGte;
    }

    // Materialize just this batch's docs from getDoc.  Field loaders need a
    // contiguous span; the per-batch alloc is what the original code did too.
    std::vector<segdoc> batchSegDocs;
    batchSegDocs.reserve(batchSizeLocal);
    for (int64_t i = batchStart; i < batchEnd; i++) {
      batchSegDocs.push_back(getDoc(i));
    }
    std::span<const segdoc> segDocs(batchSegDocs);
    int columnSize = (int)segDocs.size();
    docListProto.row_count = columnSize;
    if (columnSize > 0 && !fieldMetadataResolved) {
      resolveReturnFieldMetadata(*req.reader, fields);
      fieldMetadataResolved = true;
    }
    if (metadata) metadata(docListProto, batchStart, batchEnd, mr);

    // An isolated context: cancellation of the request group, or of whatever
    // context a resumed emitter runs in, must not skip loaders and leave a
    // batch's columns unfilled. Only a loader's own exception ends the wait.
    std::optional<oneapi::tbb::task_group_context> loadColumnsContext;
    std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
    oneapi::tbb::task_group* tg = nullptr;
    if (parallel) {
      auto& context = loadColumnsContext.emplace(oneapi::tbb::task_group_context::isolated);
      tg = &loadColumnsTaskGroup.emplace(context);
    }

    bool returnScores = getScores;

    // indirect sort the documents so we can access them in order of both segment and docid
    std::vector<RowIndex> sortedIdx(segDocs.size());
    std::iota(sortedIdx.begin(), sortedIdx.end(), 0);
    std::ranges::sort(sortedIdx, [&segDocs](auto a, auto b) {
      return segDocs[a] < segDocs[b];
    });

    // calculate segment run lengths so they can be reused when retrieving each column.
    auto bySeg = sortedIdx
                 | std::views::chunk_by([&segDocs](auto a, auto b) {
      return segDocs[a].segment() == segDocs[b].segment();
    })
                 | std::views::transform([](const auto& run) { return (RowIndex)run.size(); });

    std::vector<std::span<RowIndex>> runs;
    RowIndex start = 0;
    for (RowIndex length : bySeg) {
      runs.push_back(std::span<RowIndex>(sortedIdx).subspan(start, length));
      start += length;
    }

    // Columns placeBatchColumns takes out of the response for the post-wait
    // scatter into docListProto.docs.  The loaders themselves are
    // placement-blind.
    SearchResponse::ColumnsType rowColumns;

    // Single-valued columns register here so the post-wait finish pass can
    // pick each column's exact missing_val and fill the missing slots.
    PendingCols pendingCols;

    // Stored-field retrieval is grouped by resource so that multiple stored
    // fields sharing one resource decompress each chunk only once.
    struct StoredGroup {
      IndexReader::FieldSegments infos;
      std::vector<StoredReq> reqs;
    };
    boost::unordered_flat_map<std::string_view, StoredGroup,
                              PackedTermHash, PackedTermEqual> storedByResource;
    // Groups served from their columns instead, and the rows of the group
    // being re-read from the chunk after the column loads (see below).
    std::vector<StoredGroup> columnGroups;
    std::vector<RowIndex> fittedIdx;
    std::vector<std::span<RowIndex>> fittedRuns;
    FieldLoaders loaders;
    auto* pendingLoaders = tg ? &loaders : nullptr;

    // Cancel and drain before unwinding destroys loader captures. This guard
    // follows the batch storage and loaders so it is destroyed first.
    // Disarmed after each wait below.
    struct TgJoinGuard {
      oneapi::tbb::task_group* tg;
      ~TgJoinGuard() {
        if (tg) {
          tg->cancel();
          try { tg->wait(); } catch (...) {}
        }
      }
    } tgJoin{tg};

    // Response column i comes from columnFields[i]: fields add the first
    // columns, _score_ the last.
    assert(docListProto.columns.empty());
    columnFields.clear();
    columnPresent.clear();
    for (const ReturnField& rf : fields) {
      size_t columnsBefore = docListProto.columns.size();
      size_t pendingBefore = pendingCols.size();
      auto outputColumn = [&]() -> luxir::api::Column& {
        return build::columnSlot(docListProto.columns, colCap, rf.outputKey, mr);
      };
      auto registerColumn = [&] {
        if (docListProto.columns.size() > columnsBefore) {
          // A field registers at most one PendingCol, for its own column.
          columnFields.push_back(&rf);
          columnPresent.push_back(pendingCols.size() > pendingBefore ? pendingCols.back().present.data() : nullptr);
        }
      };

      switch (rf.kind) {
        case ReturnField::Kind::SCORE:
          returnScores = true;
          continue;
        case ReturnField::Kind::VERSION:
          loadVersionRuns(req, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          registerColumn();
          continue;
        case ReturnField::Kind::PHYSICAL:
          break;
      }
      std::string_view field = rf.physicalName;
      FieldType& fieldType = *rf.fieldType;

      // Group stored fields by resource. The group can use columns only
      // when every column preserves the source value; otherwise decompress
      // each chunk once for all outputs.
      auto recordStoredReq = [&](std::string_view resourceName) {
        std::span<std::string_view> starget;
        std::span<luxir::api::ArrStr> mtarget;
        auto& fieldCol = outputColumn();
        uint8_t* present = allocStringColumn(fieldCol, columnSize, fieldType.multiValued(),
                                             starget, mtarget, pendingCols, mr);
        auto& group = storedByResource[resourceName];
        group.infos = rf.storedInfos;
        group.reqs.push_back({field, &fieldType, rf.infos, fieldType.multiValued(), starget, mtarget, present});
      };

      switch (fieldType.type()) {
        case FieldType::Type::INT: {
          loadNumCol<IntColEmit>(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          break;
        }
        case FieldType::Type::FLOAT: {
          loadNumCol<FloatColEmit>(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          break;
        }
        case FieldType::Type::DOUBLE: {
          loadNumCol<DoubleColEmit>(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          break;
        }
        case FieldType::Type::DATE: {
          // DATE is epoch millis in the int column; emit the raw millis as
          // col_i.  ISO-8601 string rendering is an input-side / JSON-layer
          // concern, not the typed gRPC column.
          loadNumCol<IntColEmit>(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          break;
        }
        case FieldType::Type::ID:
        case FieldType::Type::STRING: {
          if (fieldType.isStored()) {
            recordStoredReq(fieldType.storedResource_);
          } else if (fieldType.hasColumn() && columnSize != 0) {
            loadStrCol(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, pendingCols, mr);
          }
          break;
        }
        case FieldType::Type::TEXT: {
          if (fieldType.isStored()) {
            recordStoredReq(fieldType.storedResource_);
          }
          break;
        }
        case FieldType::Type::VECTOR: {
          if (columnSize != 0) {
            loadVectorCol(req, rf.infos, fieldType, segDocs, runs, outputColumn(), pendingLoaders, mr);
          }
          break;
        }
        default:
          break;
      }
      registerColumn();
    }

    for (auto& [resourceName, group] : storedByResource) {
      auto& reqs = group.reqs;
      // A single-valued STRING or ID column holds the source value except
      // where a normalizer rewrote it (every row) or the term was fitted to
      // the term space (rows recognizable by length, re-read from the chunk
      // after the loads). Multi-valued columns can differ from stored source,
      // and TEXT has no column, so such members send the group to the chunk.
      bool useColumns = std::ranges::all_of(reqs, [](const StoredReq& r) {
        if (r.multi || !r.fieldType->hasColumn()) return false;
        switch (r.fieldType->type()) {
          case FieldType::ID: return true;
          case FieldType::STRING: return !static_cast<StrFieldType*>(r.fieldType)->normalizer;
          default: return false;
        }
      });
      if (useColumns) {
        for (const auto& r : reqs) {
          loadStrColWithTargets(req, r.infos, *r.fieldType, r.starget, r.mtarget, r.present,
                                segDocs, runs, pendingLoaders, mr);
        }
        columnGroups.push_back(std::move(group));
      } else {
        loadStoredFields(req, group.infos, std::move(reqs),
                         segDocs, runs, pendingLoaders, mr);
      }
    }

    bool scoreRows = returnScores && placement == luxir::api::DocFormat::ROWS;
    if (returnScores && !scoreRows) {
      auto& scoresProto = build::columnSlot(docListProto.columns, colCap, "_score_", mr);
      auto& floatColProto = scoresProto.kind.template emplace<luxir::api::ColFloat>();
      float* scores = build::allocArray(floatColProto.v, columnSize, mr);
      for (int i = 0; i < columnSize; i++) {
        scores[i] = getScore(batchStart + i);
      }
    }

    loadFields(tg, loaders, runs);
    if (tg != nullptr) {
      Signal::emit("emitDocsColumnsWait", &req, tg);
      tg->wait();
    }
    tgJoin.tg = nullptr;  // joined normally

    // A column value of PackedTerm::MIN_FITTED_LEN bytes or more may be a
    // term fitted to the term space rather than the source (or a genuine
    // value of that length); re-read those rows from the stored chunk.  The
    // subset keeps sortedIdx's segment order, so loadStoredFields serves it
    // as a small batch with its own segment runs.
    for (auto& group : columnGroups) {
      auto& reqs = group.reqs;
      loaders.clear();
      fittedIdx.clear();
      fittedRuns.clear();
      for (RowIndex idx : sortedIdx) {
        bool fitted = std::ranges::any_of(reqs, [idx](const StoredReq& r) {
          return r.present[idx] && PackedTerm::mayBeFitted(r.starget[idx]);
        });
        if (fitted) fittedIdx.push_back(idx);
      }
      start = 0;
      while (start < fittedIdx.size()) {
        RowIndex end = start + 1;
        while (end < fittedIdx.size()
               && segDocs[fittedIdx[end]].segment() == segDocs[fittedIdx[start]].segment()) end++;
        fittedRuns.push_back(std::span<RowIndex>(fittedIdx).subspan(start, end - start));
        start = end;
      }
      if (fittedIdx.empty()) continue;
      tgJoin.tg = tg;
      loadStoredFields(req, group.infos, std::move(reqs), segDocs, fittedRuns, pendingLoaders, mr);
      loadFields(tg, loaders, fittedRuns);
      if (tg != nullptr) {
        Signal::emit("emitDocsStoredWait", &req, tg);
        tg->wait();
      }
      tgJoin.tg = nullptr;
    }

    // All slots are loaded; pick each single-valued column's exact
    // missing_val and fill its missing slots.  (ROWS mode relies on the
    // exact sentinels for its presence checks.)
    finishPendingCols(pendingCols);
    placeBatchColumns(placement, columnFields, columnPresent, docListProto.columns, rowColumns,
                      (size_t)columnSize, scoreRows, columnFate, rowCaps, mr);

    // Scores bypass the row columns: every doc has one, and a direct
    // write avoids the all-present column's sentinel ambiguity (a real
    // 0.0 score would collide with ColFloat's default missing_val).
    if (!rowCaps.empty() && columnSize > 0) {
      auto* rows = scatterColumnsToRows(rowColumns, docListProto, (size_t)columnSize, rowCaps, mr);
      if (scoreRows) {
        for (int i = 0; i < columnSize; i++) {
          build::mapSlot<luxir::api::Val>(rows[i].fields, rowCaps[i], "_score_", mr)->kind =
              getScore(batchStart + i);
        }
      }
    }

    batchStart = batchEnd;
    if (!lastResponse) {
      batchArenaGuard.arena = nullptr;  // reply() owns the arena release from here
      switch (req.reply(response)) {
        case SearchRequest::ReplyStatus::OK:
          break;
        case SearchRequest::ReplyStatus::PAUSE:
          streamPauseCount++;
          req.resumeWhenDrained([this] { produce(); });
          return false;  // paused: the stream is NOT over; resume re-enters produce()
        case SearchRequest::ReplyStatus::CANCEL:
          batchStart = numCollected;  // client is gone; skip the remaining batches
          break;
      }
    }
  } while (batchStart < numCollected);
  return true;
}

// Resolve the request's returned-field selectors.  An empty list projects the
// default set: every logical root in the reader's retrievable catalog, "id"
// first, the rest in name order.  A selector containing '*' is a pattern: it
// expands in place to the catalog names it matches (name order).  A pattern
// containing "__" expands over derived representations (variants with a
// column, by physical name); any other pattern expands over logical roots.
// So patterns never surface vectors, geo, or engine '_' names, "__self" (a
// selector alias, not a representation), or variants unless asked for, and a
// pattern matching nothing contributes nothing, never an error.  Explicit
// names may name anything, including _version_ and vectors; an unknown name
// throws here, before any batch is assembled.  Explicit wins on collision: a
// name also given explicitly anywhere in the list is skipped by every
// pattern, a name matched by several patterns is kept once, at its first
// match, and a repeated explicit name collapses to its first occurrence (a
// repeat would re-emplace the same output Column and orphan the earlier
// stored-field target).  Entries from patterns or an empty list are marked
// discovered.  The plan is allocated from the protobuf request arena: emission
// starts on task-group threads and concurrent ops must not race (protobuf
// Arena allocation is thread-safe; the request MemPool is not).  The names
// are reader-catalog or request-proto views; generated read names are copied
// into the request arena. The reader and schema are pinned for the request.
inline std::span<ReturnField> resolveReturnFields(SearchRequest& req,
                                                  std::span<const std::string_view> fields) {
  auto isPattern = [](std::string_view f) { return f.find('*') != std::string_view::npos; };
  ArenaResource mr(&req.arena);
  std::vector<ReturnField> picked;
  auto pushDiscovered = [&](const IndexReader::RetrievableField& field) {
    picked.push_back({field.name, field.name, field.type, ReturnField::Kind::PHYSICAL, true});
  };
  auto pushExplicit = [&](std::string_view f) {
    for (auto& rf : picked) {
      if (rf.outputKey == f) return;  // explicit lists are small; linear scan
    }
    if (f == "_score_") {
      picked.push_back({f, {}, nullptr, ReturnField::Kind::SCORE, false});
      return;
    }
    auto source = req.schema->resolveFor(f, OpClass::RETRIEVE);
    if (source.fieldType->type() == FieldType::TEXT && !source.fieldType->isStored()) {
      auto* primary = source.owner ? source.owner->primary.get() : source.fieldType;
      std::string message = "Field '" + std::string(f) + "' has no retrievable value";
      bool storedSource = primary->isStored() && (primary->type() == FieldType::TEXT ||
          primary->type() == FieldType::STRING || primary->type() == FieldType::ID);
      if (storedSource || primary->hasColumn()) {
        message += "; retrieve logical root '" + source.logicalName + "' instead";
      } else if (primary->type() == FieldType::TEXT) {
        message += "; logical root '" + source.logicalName + "' must enable stored for source text";
      }
      throw RequestError(message, "invalid_field");
    }
    auto kind = source.physicalName == "_version_" ? ReturnField::Kind::VERSION : ReturnField::Kind::PHYSICAL;
    picked.push_back({f, build::arenaStr(mr, source.physicalName), source.fieldType, kind, false});
  };
  if (fields.empty()) {
    auto catalog = req.reader->retrievableFields();
    picked.reserve(catalog.size());
    for (const auto& field : catalog) {
      if (!field.derived) pushDiscovered(field);
    }
    auto idIt = std::ranges::find(picked, std::string_view("id"), &ReturnField::outputKey);
    if (idIt != picked.end()) std::rotate(picked.begin(), idIt, idIt + 1);
  } else if (std::ranges::none_of(fields, isPattern)) {
    picked.reserve(fields.size());
    for (std::string_view f : fields) pushExplicit(f);
  } else {
    auto catalog = req.reader->retrievableFields();
    boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual> taken;
    for (std::string_view f : fields) {
      if (!isPattern(f)) taken.insert(f);
    }
    for (std::string_view f : fields) {
      if (!isPattern(f)) {
        pushExplicit(f);
        continue;
      }
      forEachFieldPattern(catalog, f, [](const auto& field) { return field.name; },
          [](const auto& field) { return field.derived; }, [&](const auto& field) {
            if (taken.insert(field.name).second) pushDiscovered(field);
          });
    }
  }
  if (picked.empty()) return {};
  auto* out = (ReturnField*)req.arena.AllocateAligned(sizeof(ReturnField) * picked.size(),
                                                      alignof(ReturnField));
  std::uninitialized_copy(picked.begin(), picked.end(), out);
  return {out, picked.size()};
}

// Entry point shared by TopDocsReq and FusionOp: creates a request-arena-owned
// emitter and produces batches until done or paused.  See DocEmitter above for
// the resumable-emission contract.
template <typename GetDocList, typename GetDoc, typename GetScore>
void emitDocsResponse(SearchRequest& req,
                      GetDocList&& getDocList,
                      int64_t numCollected,
                      GetDoc&& getDoc,
                      GetScore&& getScore,
                      int64_t totalHits,
                      std::span<const std::string_view> fields,
                      int32_t batchSize,
                      int64_t offset,
                      bool emitCount,
                      bool getScores,
                      luxir::api::DocFormat docFormat,
                      DocEmission emission,
                      DocBatchMetadata metadata = {},
                      bool foundGte = false)
{
  int32_t maxBatchSize = batchSize;
  if (emission == DocEmission::FINAL_RESPONSE) {
    int64_t numEmitted = numCollected - std::min(offset, numCollected);
    if (numEmitted > std::numeric_limits<int32_t>::max()) {
      throw std::length_error("too many documents for one response batch");
    }
    maxBatchSize = (int32_t)std::max<int64_t>(numEmitted, 1);
  } else if (maxBatchSize <= 0) {
    maxBatchSize = 100;  // what should the default be?
  } else if (maxBatchSize > 256) {
    maxBatchSize = 256;
  }

  // Resolve selectors: empty projects the default set, '*' entries expand
  // against the reader's catalog (see resolveReturnFields).  Every field
  // loads as a column and placeBatchColumns settles each batch: ROWS puts
  // every field in docs, COLUMNS leaves out all-missing discovered columns,
  // ADAPTIVE picks per field by encoded size.  _score_ (asked for by
  // get_scores) is a column except under ROWS.
  std::span<ReturnField> rfields = resolveReturnFields(req, fields);

  // DEFAULT resolves to the transport's preference (HTTP -> ROWS, gRPC ->
  // COLUMNS); an out-of-range wire value falls back to the default too.
  luxir::api::DocFormat placement = docFormat;
  if (placement != luxir::api::DocFormat::ROWS && placement != luxir::api::DocFormat::COLUMNS
      && placement != luxir::api::DocFormat::ADAPTIVE) {
    placement = req.docFormatDefault == luxir::api::DocFormat::ROWS
        ? luxir::api::DocFormat::ROWS : luxir::api::DocFormat::COLUMNS;
  }

  // Response columns map capacity (columnSlot allocates the whole backing
  // array on first insertion, so the cap is paid per batch): every field
  // plus the synthetic _score_.
  size_t colCap = 1;
  for (const auto& rf : rfields) {
    if (rf.kind != ReturnField::Kind::SCORE) colCap++;
  }

  using Impl = DocEmitterImpl<std::decay_t<GetDocList>, std::decay_t<GetDoc>,
                              std::decay_t<GetScore>>;
  auto* emitter = luxir::arenaCreate<Impl>(req.arena, req,
      std::forward<GetDocList>(getDocList), std::forward<GetDoc>(getDoc),
      std::forward<GetScore>(getScore), numCollected, totalHits, rfields,
      maxBatchSize, offset, emitCount, foundGte, getScores, placement,
      /*parallel=*/req.tg != nullptr, colCap, std::move(metadata));
  req.streamStarted();
  emitter->produce();
}

} // namespace luxir
