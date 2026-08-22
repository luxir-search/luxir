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
#include <memory_resource>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/reader/FieldReader.h"
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
#include "luxir/util/luxir_util.h"
#include "luxir/util/thread.h"

namespace luxir {

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
inline void loadStrColForSegment(SearchRequest& req, std::string_view field, FieldType& fieldType,
                                 std::span<uint8_t> idxSpan, std::span<const segdoc> segDocs,
                                 std::span<std::string_view> starget, std::span<luxir::api::ArrStr> mtarget,
                                 uint8_t* present, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(postingsReader);
  bool found = fieldReader.seek(field);
  if (!found) {
    return;
  }
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);

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
inline void loadStrColWithTargets(SearchRequest& req, std::string_view field, FieldType& fieldType,
                                  std::span<std::string_view> starget, std::span<luxir::api::ArrStr> mtarget,
                                  uint8_t* present,
                                  std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                                  const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg,
                                  std::pmr::memory_resource& mr)
{
  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, field, &fieldType, segDocs, starget, mtarget, present, &mr]() {
      loadStrColForSegment(req, field, fieldType, idxSpan, segDocs, starget, mtarget, present, mr);
    });
    start += runlen;
  }
}

inline void loadStrCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                       std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                       const std::span<uint8_t> segRunLength,
                       SearchResponse::ColumnsType& columnsProto, size_t colCap,
                       oneapi::tbb::task_group* tg, PendingCols& pendingCols, std::pmr::memory_resource& mr)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;
  std::span<std::string_view> starget;
  std::span<luxir::api::ArrStr> mtarget;
  auto& fieldCol = build::columnSlot(columnsProto, colCap, field, mr);
  uint8_t* present = allocStringColumn(fieldCol, columnSize, fieldType.multiValued(),
                                       starget, mtarget, pendingCols, mr);
  loadStrColWithTargets(req, field, fieldType, starget, mtarget, present, segDocs, sortedIdx, segRunLength, tg, mr);
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

// We are guaranteed that the resources passed here will remain valid for any subtasks added to "tg" (i.e. the
// caller waits on "tg" before releasing the resources).
template <typename Emit>
inline void loadNumCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                       std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                       const std::span<uint8_t> segRunLength,
                       SearchResponse::ColumnsType& columnsProto, size_t colCap,
                       oneapi::tbb::task_group* tg, PendingCols& pendingCols, std::pmr::memory_resource& mr)
{
  auto& fieldCol = build::columnSlot(columnsProto, colCap, field, mr);  // output Column slot
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
  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, field, &fieldType, segDocs, starget, mtarget, present, &mr]() {
      auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
      auto segNum = sortedSegDocs[0].segment();
      auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

      auto& postingsReader = req.reader->segments()[segNum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      bool found = fieldReader.seek(field);
      if (!found) {
        return;
      }
      SegFieldInfo segFieldInfo;
      fieldReader.readFieldInfo(segFieldInfo);

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

    start += runlen;
  }
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

inline void loadVectorColForSegmentSingle(SearchRequest& req, std::string_view field,
                                          std::span<uint8_t> idxSpan,
                                          std::span<const segdoc> segDocs,
                                          std::span<luxir::api::Vector> target, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(postingsReader);
  if (!fieldReader.seek(field)) return;
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);

  auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes) {
    assert(segDocs[idxSpan[idx]].docId() == doc);
    unused(doc);
    fillVectorF32(target[idxSpan[idx]], bytes, mr);
  };
  StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
}

inline void loadVectorColForSegmentMulti(SearchRequest& req, std::string_view field,
                                         std::span<uint8_t> idxSpan,
                                         std::span<const segdoc> segDocs,
                                         std::span<luxir::api::ArrVector> target, std::pmr::memory_resource& mr)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(postingsReader);
  if (!fieldReader.seek(field)) return;
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);

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
inline void loadVectorCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                          std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                          const std::span<uint8_t> segRunLength,
                          SearchResponse::ColumnsType& columnsProto, size_t colCap,
                          oneapi::tbb::task_group* tg, std::pmr::memory_resource& mr)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;
  auto& fieldCol = build::columnSlot(columnsProto, colCap, field, mr);

  if (!fieldType.multiValued()) {
    auto& vecCol = fieldCol.kind.emplace<luxir::api::ColVector>();
    std::span<luxir::api::Vector> target(build::allocArray(vecCol.v, columnSize, mr), columnSize);

    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [idxSpan, &req, field, segDocs, target, &mr]() {
        loadVectorColForSegmentSingle(req, field, idxSpan, segDocs, target, mr);
      });
      start += runlen;
    }
  } else {
    auto& arrVecCol = fieldCol.kind.emplace<luxir::api::MultiVector>();
    std::span<luxir::api::ArrVector> target(build::allocArray(arrVecCol.v, columnSize, mr), columnSize);

    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [idxSpan, &req, field, segDocs, target, &mr]() {
        loadVectorColForSegmentMulti(req, field, idxSpan, segDocs, target, mr);
      });
      start += runlen;
    }
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
inline void loadStoredFields(SearchRequest& req, std::string_view resourceName,
                             std::vector<StoredReq> reqsOwned,
                             std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                             const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg,
                             std::pmr::memory_resource& mr)
{
  // Move into a shared_ptr so every segment task can safely reference the
  // same vector even after this function returns.
  auto reqs = std::make_shared<std::vector<StoredReq>>(std::move(reqsOwned));

  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, resourceName, reqs, segDocs, &mr]() {
      auto segNum = segDocs[idxSpan[0]].segment();
      auto& postingsReader = req.reader->segments()[segNum].postingsReader();
      auto sfr = StoredFieldsReader::open(postingsReader, resourceName);

      // Partition reqs: sfrReqs serve from SFR, colReqs fall back to
      // column.  The fallback is attempted for any STRING/ID field type
      // (not just fields whose current schema sets hasColumn): older
      // segments predating a schema change may still have the ord/value
      // column on disk, and loadStrColForSegment no-ops gracefully if the
      // field is actually absent.  TEXT fields have no retrievable column
      // - they stay empty when missing from the segment's SFR.
      std::vector<StoredReq*> sfrReqs;
      std::vector<StoredReq*> colReqs;
      sfrReqs.reserve(reqs->size());
      for (auto& r : *reqs) {
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
        loadStrColForSegment(req, r->fieldName, *r->fieldType, idxSpan, segDocs, r->starget, r->mtarget,
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
                break;
              }
            }
          });
        }
      }
    });
    start += runlen;
  }
}

// ROWS format: scatter loaded columns into per-doc field maps (DocList.docs).
// The loaders always fill column-shaped storage (pre-sized spans, filled in
// parallel per segment); in ROWS mode that storage is arena scratch never
// attached to the response, and this post-wait pass moves the values into
// rows.  Presence uses the exact per-column contracts the COLUMNS wire shape
// already guarantees: single-valued sentinel (missing_val, exact after
// finishPendingCols), multi-valued empty array, vector unset f32.  Missing =
// key absent; no sentinels in rows.  Values share the arena-backed spans and
// string views with the scratch columns - nothing is copied.
//
// Returns the mutable row array so the caller can add pseudo-fields
// (_score_).  fieldCap is each row map's slot capacity (distinct field
// names, including pseudo-fields the caller will add).
inline luxir::api::Map* scatterColumnsToRows(const SearchResponse::ColumnsType& cols,
                                             luxir::api::DocList& docListProto,
                                             size_t numDocs, size_t fieldCap,
                                             std::pmr::memory_resource& mr)
{
  using luxir::api::Val;
  luxir::api::Map* rows = build::allocArray(docListProto.docs, numDocs, mr);
  for (const auto& [field, col] : cols) {
    auto slot = [&](size_t i) -> Val& {
      return *build::mapSlot<Val>(rows[i].fields, fieldCap, field, mr);
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
    } else if (auto* c = std::get_if<luxir::api::ColVector>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (c->v[i].f32.has_value()) slot(i).kind = c->v[i];
    } else if (auto* c = std::get_if<luxir::api::MultiVector>(&col.kind)) {
      for (size_t i = 0; i < numDocs; i++)
        if (!c->v[i].v.empty()) slot(i).kind = c->v[i];
    }
    // monostate can't occur here (loaders always emplace a kind when
    // numDocs > 0); ColMap/ArrVal are never produced by the doc loaders.
  }
  return rows;
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
// callbacks themselves.  The final batch is assembled into req.lastResponse
// but NOT sent here - the completion protocol on SearchRequest (streamEnded /
// bodyDone) decides who sends it.  All but the final response have more=true.
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
  std::span<const std::string_view> fields;
  int32_t maxBatchSize;
  int64_t offset;
  bool getNumber;
  bool getScores;
  // Placement.  rowsMode: requested fields go to per-document maps
  // (DocList.docs) instead of dense columns.  scoresInRows: the synthetic
  // _score_ goes into those maps too; otherwise it is a dense column even
  // when rowsMode holds (discovered fields in rows beside a score column).
  bool rowsMode;
  bool scoresInRows;
  // Snapshot of req.tg != nullptr: req.tg points at submit()'s stack and may
  // dangle by the time a resumed emitter runs.
  bool parallel;
  size_t colCap;
  int64_t totalBatches;
  int64_t batchStart = 0;  // resume cursor

  DocEmitterImpl(SearchRequest& req, GetDocList getDocList, GetDoc getDoc,
                 GetScore getScore, int64_t numCollected, int64_t totalHits,
                 std::span<const std::string_view> fields, int32_t maxBatchSize,
                 int64_t offset, bool getNumber, bool getScores, bool rowsMode,
                 bool scoresInRows, bool parallel, size_t colCap)
      : req(req), getDocList(std::move(getDocList)), getDoc(std::move(getDoc)),
        getScore(std::move(getScore)), numCollected(numCollected), totalHits(totalHits),
        fields(fields), maxBatchSize(maxBatchSize), offset(offset), getNumber(getNumber),
        getScores(getScores), rowsMode(rowsMode), scoresInRows(scoresInRows),
        parallel(parallel), colCap(colCap),
        // If no documents were collected, we still need to send an empty response
        totalBatches(numCollected > 0 ? numCollected : 1) {}

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
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(req.mutex);
    if (req.lastResponse->proto.error.empty()) {
      req.lastResponse->proto.error = luxir::api::build::arenaStr(req.lastResponse->mr, e.what());
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(req.mutex);
    if (req.lastResponse->proto.error.empty()) {
      req.lastResponse->proto.error = "unknown error while emitting documents";
    }
  }
  if (finished) {
    // The final batch (assembled into req.lastResponse by produceBatches) is
    // sent by the completion protocol; this may delete the request.
    req.streamEnded();
  }
}

template <typename GetDocList, typename GetDoc, typename GetScore>
bool DocEmitterImpl<GetDocList, GetDoc, GetScore>::produceBatches() {
  while (batchStart < totalBatches) {
    int64_t batchEnd = std::min(batchStart + maxBatchSize, numCollected);
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
    docListProto.offset = offset + batchStart;
    if (!lastResponse) {
      docListProto.more = true;
      response.proto.more = true;  // also set at the response level for easier client handling.
    }

    if (getNumber) {
      docListProto.found = totalHits;
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

    std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
    oneapi::tbb::task_group* tg = parallel ? &loadColumnsTaskGroup.emplace() : nullptr;

    bool returnScores = getScores;

    // indirect sort the documents so we can access them in order of both segment and docid
    std::vector<uint8_t> sortedIdx(segDocs.size()); // uint8_t works for up to 256 docs.
    std::iota(sortedIdx.begin(), sortedIdx.end(), 0);
    std::ranges::sort(sortedIdx, [&segDocs](auto a, auto b) {
      return segDocs[a] < segDocs[b];
    });

    // calculate segment run lengths so they can be reused when retrieving each column.
    auto bySeg = sortedIdx
                 | std::views::chunk_by([&segDocs](auto a, auto b) {
      return segDocs[a].segment() == segDocs[b].segment();
    })
                 | std::views::transform([](const auto& run) { return (uint8_t)run.size(); });

    std::vector<uint8_t> segRunLength;
    std::ranges::copy(bySeg, std::back_inserter(segRunLength));

    // In ROWS mode the loaders fill arena scratch columns that are never
    // attached to the response; the post-wait scatter pass moves the values
    // into docListProto.docs.  The loaders themselves are format-blind.
    SearchResponse::ColumnsType scratchColumns;
    auto& columnsProto = rowsMode ? scratchColumns : docListProto.columns;

    // Single-valued columns register here so the post-wait finish pass can
    // pick each column's exact missing_val and fill the missing slots.
    PendingCols pendingCols;

    // Stored-field retrieval is grouped by resource so that multiple stored
    // fields sharing one resource decompress each chunk only once.
    boost::unordered_flat_map<std::string_view, std::vector<StoredReq>,
                              PackedTermHash, PackedTermEqual> storedByResource;

    // If anything below throws while loader tasks are in flight (e.g. a later
    // field's schema lookup), the task_group must be joined before unwinding
    // destroys what the tasks reference - declared HERE, after every local the
    // loader tasks touch (sortedIdx, segRunLength, scratch columns, pending
    // cols, storedByResource), so its join runs first.  Disarmed after the
    // normal wait below.
    struct TgJoinGuard {
      oneapi::tbb::task_group* tg;
      ~TgJoinGuard() {
        if (tg) {
          tg->cancel();
          try { tg->wait(); } catch (...) {}
        }
      }
    } tgJoin{tg};

    for (std::string_view field: fields) {
      // should we allow _scores_ as a field name?
      if (field == "_score_") {
        returnScores = true;
        continue;
      }

      auto& fieldType = *req.schema->getFieldTypeEx(field);

      // Collect any STORED field (column or not) into the candidate map;
      // the per-resource strategy is decided after the loop.  Rationale:
      // if some field in a resource is stored-only we'll decompress the
      // chunk anyway, in which case pulling column-backed peer fields out
      // of the same chunk is cheaper than doing separate column lookups.
      auto recordStoredReq = [&](std::string_view resourceName) {
        std::span<std::string_view> starget;
        std::span<luxir::api::ArrStr> mtarget;
        auto& fieldCol = build::columnSlot(columnsProto, colCap, field, mr);
        uint8_t* present = allocStringColumn(fieldCol, columnSize, fieldType.multiValued(),
                                             starget, mtarget, pendingCols, mr);
        storedByResource[resourceName].push_back(
            {field, &fieldType, fieldType.multiValued(), starget, mtarget, present});
      };

      switch (fieldType.type()) {
        case FieldType::Type::INT: {
          loadNumCol<IntColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, pendingCols, mr);
          break;
        }
        case FieldType::Type::FLOAT: {
          loadNumCol<FloatColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, pendingCols, mr);
          break;
        }
        case FieldType::Type::DOUBLE: {
          loadNumCol<DoubleColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, pendingCols, mr);
          break;
        }
        case FieldType::Type::DATE: {
          // DATE is epoch millis in the int column; emit the raw millis as
          // col_i.  ISO-8601 string rendering is an input-side / JSON-layer
          // concern, not the typed gRPC column.
          loadNumCol<IntColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, pendingCols, mr);
          break;
        }
        case FieldType::Type::ID:
        case FieldType::Type::STRING: {
          if (fieldType.isStored()) {
            recordStoredReq(fieldType.storedResource_);
          } else if (fieldType.hasColumn()) {
            loadStrCol(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, pendingCols, mr);
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
          loadVectorCol(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, colCap, tg, mr);
          break;
        }
        default:
          break;
      }
    }

    // Per-resource strategy.  If any field in the group is stored-only we
    // decompress the resource's chunks regardless, so hand the whole
    // group to loadStoredFields and let it read every field from the
    // chunk.  If all fields in the group have columns available, skip
    // stored-fields entirely and read from columns - faster, no
    // decompression.
    for (auto& [resourceName, reqs] : storedByResource) {
      bool anyStoredOnly = std::ranges::any_of(reqs, [](const StoredReq& r) {
        return !r.fieldType->hasColumn();
      });
      if (anyStoredOnly) {
        loadStoredFields(req, resourceName, std::move(reqs),
                         segDocs, sortedIdx, segRunLength, tg, mr);
      } else {
        for (auto& r : reqs) {
          loadStrColWithTargets(req, r.fieldName, *r.fieldType, r.starget, r.mtarget, r.present,
                                segDocs, sortedIdx, segRunLength, tg, mr);
        }
      }
    }

    if (returnScores && !scoresInRows) {
      auto& scoresProto = build::columnSlot(docListProto.columns, colCap, "_score_", mr);
      auto& floatColProto = scoresProto.kind.template emplace<luxir::api::ColFloat>();
      float* scores = build::allocArray(floatColProto.v, columnSize, mr);
      for (int i = 0; i < columnSize; i++) {
        scores[i] = getScore(batchStart + i);
      }
    }

    if (tg != nullptr) {
      tg->wait();
    }
    tgJoin.tg = nullptr;  // joined normally

    // All slots are loaded; pick each single-valued column's exact
    // missing_val and fill its missing slots.  (ROWS mode relies on the
    // exact sentinels for its presence checks.)
    finishPendingCols(pendingCols);

    if (rowsMode && columnSize > 0) {
      // Scores bypass the scratch column: every doc has one, and a direct
      // write avoids the all-present column's sentinel ambiguity (a real
      // 0.0 score would collide with ColFloat's default missing_val).
      bool scoreRows = returnScores && scoresInRows;
      size_t fieldCap = scratchColumns.size() + (scoreRows ? 1 : 0);
      auto* rows = scatterColumnsToRows(scratchColumns, docListProto, (size_t)columnSize, fieldCap, mr);
      if (scoreRows) {
        for (int i = 0; i < columnSize; i++) {
          build::mapSlot<luxir::api::Val>(rows[i].fields, fieldCap, "_score_", mr)->kind =
              getScore(batchStart + i);
        }
      }
    }

    batchStart += maxBatchSize;
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
          batchStart = totalBatches;  // client is gone; skip the remaining batches
          break;
      }
    }
  }
  return true;
}

// The projection for a request that names no fields: every field the reader
// can project - column-backed scalars and strings, and stored values - except
// vectors (large and rarely wanted unasked), geo points (not yet projectable),
// and engine-reserved names (leading underscore, i.e. _version_).  Discovery
// is physical (IndexReader::projectableFields, union over segments) and then
// filtered through the request's schema, so a name the schema no longer knows
// or no longer marks retrievable is dropped, never thrown on.  "id" leads,
// the rest are alphabetical.  The returned views are the reader's catalog
// views, which req.reader pins for the request's lifetime.
inline std::span<const std::string_view> defaultProjection(SearchRequest& req) {
  auto catalog = req.reader->projectableFields();
  std::vector<std::string_view> picked;
  picked.reserve(catalog.size());
  for (std::string_view name : catalog) {
    if (name.empty() || name[0] == '_') continue;
    FieldType* fieldType = req.schema->getFieldTypePtr(name);
    if (fieldType == nullptr) continue;
    switch (fieldType->type()) {
      case FieldType::Type::INT:
      case FieldType::Type::FLOAT:
      case FieldType::Type::DOUBLE:
      case FieldType::Type::DATE:
      case FieldType::Type::STRING:
      case FieldType::Type::ID:
      case FieldType::Type::TEXT:
        break;
      default:
        continue;
    }
    if (!fieldType->hasColumn() && !fieldType->isStored()) continue;
    picked.push_back(name);
  }
  auto idIt = std::ranges::find(picked, std::string_view("id"));
  if (idIt != picked.end()) std::rotate(picked.begin(), idIt, idIt + 1);
  auto out = req.requestPool.make_span<std::string_view>(picked.size());
  std::ranges::copy(picked, out.begin());
  return out;
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
                      bool getNumber,
                      bool getScores,
                      luxir::api::DocFormat docFormat)
{
  int32_t maxBatchSize = batchSize;
  if (maxBatchSize <= 0) {
    maxBatchSize = 100;  // what should the default be?
  } else if (maxBatchSize > 256) {
    maxBatchSize = 256;
  }

  // No fields named: project the default set.
  bool discovered = fields.empty();
  if (discovered) fields = defaultProjection(req);

  // DEFAULT resolves to the transport's preference (HTTP -> ROWS, gRPC ->
  // COLUMNS); an out-of-range wire value falls back to the default too.
  bool formatRows = docFormat == luxir::api::DocFormat::ROWS
                    || (docFormat != luxir::api::DocFormat::COLUMNS
                        && req.docFormatDefault == luxir::api::DocFormat::ROWS);
  // DocList placement is explicitly requested -> dense columns, discovered ->
  // docs[i]: the column set is a property of the request, never of what the
  // index happens to hold.  So discovered fields land in rows whatever
  // document_format says, while _score_ (asked for by get_scores) keeps the
  // format's placement - a column under columns format, beside the rows.
  bool rowsMode = discovered || formatRows;
  bool scoresInRows = formatRows;

  // Upper bound on distinct response columns: one per requested field plus the
  // synthetic _score_ column. Used to pre-size the columns map's slot array.
  size_t colCap = fields.size() + 1;

  using Impl = DocEmitterImpl<std::decay_t<GetDocList>, std::decay_t<GetDoc>,
                              std::decay_t<GetScore>>;
  auto* emitter = luxir::arenaCreate<Impl>(req.arena, req,
      std::forward<GetDocList>(getDocList), std::forward<GetDoc>(getDoc),
      std::forward<GetScore>(getScore), numCollected, totalHits, fields,
      maxBatchSize, offset, getNumber, getScores, rowsMode, scoresInRows,
      /*parallel=*/req.tg != nullptr, colCap);
  req.streamStarted();
  emitter->produce();
}

} // namespace luxir
