#pragma once

// Field loading + streaming response emission for an already-ranked list of
// (segdoc, score) pairs.  Used by TopDocsReq (after collector sort) and by
// FusionOp (after RRF merge); both share the same segment-grouped column
// loader, batched protobuf assembly, and streaming reply path.
//
// The per-field load helpers (loadNumCol/loadStrCol/loadVectorCol/
// loadStoredFields) are pure functions of (req, field, segDocs, sortedIdx,
// segRunLength, columnsProto, tg) - they were lifted out of TopDocsReq
// unchanged.

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "protos/solux_types.pb.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/search/Collector.h"
#include "solux/search/SearchRequest.h"
#include "solux/util/MemPool.h"
#include "solux/util/NumericUtils.h"
#include "solux/util/StrRef.h"
#include "solux/util/solux_util.h"
#include "solux/util/thread.h"

namespace solux {

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
  std::span<std::string*> starget;
  std::span<solux::proto::ArrStr*> mtarget;
  uint8_t* present = nullptr;  // presence slots when single-valued (see PendingCol)
};

// Allocate the output Column (col_s or multi_s) for a TEXT/STRING field
// and return spans into its internal storage.  Used by both column
// retrieval (loadStrCol) and stored retrieval - the output proto shape is
// identical in both cases.  Leaves spans empty when columnSize is 0.
// Returns the presence slots for a single-valued column (null for multi,
// which signals missing structurally).
inline uint8_t* allocStringColumn(solux::proto::Column& fieldCol, size_t columnSize, bool multi,
                                  std::span<std::string*>& starget,
                                  std::span<solux::proto::ArrStr*>& mtarget,
                                  PendingCols& pendingCols)
{
  if (columnSize == 0) return nullptr;
  if (!multi) {
    auto& strCol = *fieldCol.mutable_col_s();
    auto& stringsProto = *strCol.mutable_v();
    stringsProto.Reserve(columnSize);
    for (auto i = 0u; i < columnSize; i++) stringsProto.Add("");
    starget = {stringsProto.mutable_data(), (size_t)columnSize};

    auto& pending = pendingCols.emplace_back();
    pending.present.assign(columnSize, 0);
    pending.finish = [&strCol](std::span<const uint8_t> present) {
      auto& v = *strCol.mutable_v();
      // "" is the default filler; only when a real empty string occurs must
      // a different filler be used: one byte past the largest present value
      // is greater than every present value, so it cannot collide.
      bool emptyPresent = false;
      for (size_t i = 0; i < present.size(); i++) {
        if (present[i] && v[(int)i].empty()) {
          emptyPresent = true;
          break;
        }
      }
      if (!emptyPresent) return;
      std::string_view maxStr;
      for (size_t i = 0; i < present.size(); i++) {
        if (present[i] && std::string_view(v[(int)i]) > maxStr) maxStr = v[(int)i];
      }
      std::string filler = std::string(maxStr) + '\0';
      strCol.set_missing_val(filler);
      for (size_t i = 0; i < present.size(); i++) {
        if (!present[i]) *v.Mutable((int)i) = filler;
      }
    };
    return pending.present.data();
  } else {
    auto& strCol = *fieldCol.mutable_multi_s();
    auto& arrArrProto = *strCol.mutable_v();
    arrArrProto.Reserve(columnSize);
    for (auto i = 0u; i < columnSize; i++) arrArrProto.Add();
    mtarget = {arrArrProto.mutable_data(), columnSize};
    return nullptr;
  }
}

// Per-segment body of loadStrCol.  Reads the field's column for one
// segment and writes values into starget (single) or mtarget (multi).
// No-op if the field isn't in the segment.  Extracted so the stored-fields
// path can fall back here when a segment predates the STORED flag.
inline void loadStrColForSegment(SearchRequest& req, std::string_view field, FieldType& fieldType,
                                 std::span<uint8_t> idxSpan, std::span<const segdoc> segDocs,
                                 std::span<std::string*> starget, std::span<solux::proto::ArrStr*> mtarget,
                                 uint8_t* present)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(poolGuard.pool(), postingsReader);
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
      auto valHandler = [&](size_t idx, int32_t doc, int64_t val) {
        assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
        tenum.seekOrd((int32_t) val - 1);
        *starget[idxSpan[idx]] = (std::string_view) tenum.term();
        if (present) present[idxSpan[idx]] = 1;
      };
      IntColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    } else {
      auto valHandler = [&](size_t idx, int32_t doc, std::string_view val) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        *starget[idxSpan[idx]] = std::string(val);
        if (present) present[idxSpan[idx]] = 1;
      };
      StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    }
  } else {
    if (isIndexedString) {
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      auto valHandler = [&](size_t idx, int32_t doc, int64_t val, int64_t valIdx, int64_t numVals) {
        assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
        solux::proto::ArrStr& target = *mtarget[idxSpan[idx]];
        if (valIdx == 0) target.mutable_v()->Reserve(numVals);
        tenum.seekOrd((int32_t) val - 1);
        auto v = (std::string_view) tenum.term();
        auto* strProto = target.mutable_v()->Add();
        *strProto = v;
      };
      IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    } else {
      auto valHandler = [&](size_t idx, int32_t doc, std::string_view val, int64_t valIdx, int64_t numVals) {
        assert(segDocs[idxSpan[idx]].docId() == doc);
        solux::proto::ArrStr& target = *mtarget[idxSpan[idx]];
        if (valIdx == 0) target.mutable_v()->Reserve(numVals);
        auto* strProto = target.mutable_v()->Add();
        *strProto = std::string(val);
      };
      StrColReader::getMultiValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
    }
  }
}

// loadStrCol when the output spans are already allocated (by the caller's
// earlier allocStringColumn).  Dispatches one per-segment task per run.
inline void loadStrColWithTargets(SearchRequest& req, std::string_view field, FieldType& fieldType,
                                  std::span<std::string*> starget, std::span<solux::proto::ArrStr*> mtarget,
                                  uint8_t* present,
                                  std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                                  const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg)
{
  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, field, &fieldType, segDocs, starget, mtarget, present]() {
      loadStrColForSegment(req, field, fieldType, idxSpan, segDocs, starget, mtarget, present);
    });
    start += runlen;
  }
}

inline void loadStrCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                       std::span<const segdoc> segDocs, std::span<uint8_t> sortedIdx,
                       const std::span<uint8_t> segRunLength,
                       SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg,
                       PendingCols& pendingCols)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;
  std::span<std::string*> starget;
  std::span<solux::proto::ArrStr*> mtarget;
  uint8_t* present = allocStringColumn(columnsProto[field], columnSize, fieldType.multiValued(),
                                       starget, mtarget, pendingCols);
  loadStrColWithTargets(req, field, fieldType, starget, mtarget, present, segDocs, sortedIdx, segRunLength, tg);
}

// Per-type policies for emitting numeric columns.  INT columns hold the
// value directly; FLOAT/DOUBLE columns hold sortable bits (see
// util/NumericUtils.h) that decode() turns back into the client-facing
// floating point value.  encodeVal/decodeEnc map client-facing values to
// the order-preserving int64 space pickMissingVal searches in (identity
// for ints, sortable bits for floats/doubles).
struct IntColEmit {
  using value_type = int64_t;
  using arr_type = solux::proto::ArrInt;
  static constexpr bool foldZeros = false;
  static auto& singleCol(solux::proto::Column& col) { return *col.mutable_col_i(); }
  static auto& multiCol(solux::proto::Column& col) { return *col.mutable_multi_i(); }
  static int64_t decode(int64_t raw) { return raw; }
  static int64_t encodeVal(int64_t v) { return v; }
  static int64_t decodeEnc(int64_t e) { return e; }
  static int64_t encLo() { return std::numeric_limits<int64_t>::min(); }
  static int64_t encHi() { return std::numeric_limits<int64_t>::max(); }
};

struct FloatColEmit {
  using value_type = float;
  using arr_type = solux::proto::ArrFloat;
  static constexpr bool foldZeros = true;
  static auto& singleCol(solux::proto::Column& col) { return *col.mutable_col_f(); }
  static auto& multiCol(solux::proto::Column& col) { return *col.mutable_multi_f(); }
  static float decode(int64_t raw) { return sortableInt32ToFloat((int32_t)raw); }
  static int64_t encodeVal(float v) { return (int64_t)floatToSortableInt32(v); }
  static float decodeEnc(int64_t e) { return sortableInt32ToFloat((int32_t)e); }
  static int64_t encLo() { return (int64_t)floatToSortableInt32(-std::numeric_limits<float>::infinity()); }
  static int64_t encHi() { return (int64_t)floatToSortableInt32(std::numeric_limits<float>::infinity()); }
};

struct DoubleColEmit {
  using value_type = double;
  using arr_type = solux::proto::ArrDouble;
  static constexpr bool foldZeros = true;
  static auto& singleCol(solux::proto::Column& col) { return *col.mutable_col_d(); }
  static auto& multiCol(solux::proto::Column& col) { return *col.mutable_multi_d(); }
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
                       SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg,
                       PendingCols& pendingCols)
{
  auto& fieldCol = columnsProto[field];  // output Column in the protobuf
  std::span<typename Emit::value_type> starget; // single valued target
  std::span<typename Emit::arr_type*> mtarget;  // multi-valued target
  uint8_t* present = nullptr;
  auto columnSize = segDocs.size();

  if (columnSize == 0) {
    return;
  }

  if (!fieldType.multiValued()) {
    auto& numCol = Emit::singleCol(fieldCol);
    auto& valsProto = *numCol.mutable_v();
    valsProto.Resize(columnSize, (typename Emit::value_type)0);
    starget = {valsProto.mutable_data(), (size_t)columnSize};
    assert(&valsProto[columnSize - 1] >= starget.data() && &valsProto[columnSize - 1] < starget.data() + columnSize);

    auto& pending = pendingCols.emplace_back();
    pending.present.assign(columnSize, 0);
    present = pending.present.data();
    pending.finish = [&numCol](std::span<const uint8_t> present) {
      auto* data = numCol.mutable_v()->mutable_data();
      size_t n = present.size();
      auto filler = pickMissingVal<Emit>({data, n}, present);
      numCol.set_missing_val(filler);
      for (size_t i = 0; i < n; i++) {
        if (!present[i]) data[i] = filler;
      }
    };
  } else {
    auto& numCol = Emit::multiCol(fieldCol);
    auto& arrArrProto = *numCol.mutable_v();
    arrArrProto.Reserve(columnSize);
    for (auto i = 0u; i < columnSize; i++) {
      arrArrProto.Add();
    }
    typename Emit::arr_type** arrstart = arrArrProto.mutable_data();
    assert(&arrArrProto.Get(columnSize - 1) == arrstart[columnSize - 1]);
    mtarget = {arrstart, columnSize};
  }
  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, field, &fieldType, segDocs, starget, mtarget, present]() {
      auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
      auto segNum = sortedSegDocs[0].segment();
      auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

      auto& postingsReader = req.reader->segments()[segNum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
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
          typename Emit::arr_type& target = *mtarget[idxSpan[idx]];
          if (valIdx == 0) {
            target.mutable_v()->Reserve(numVals);
          }
          target.mutable_v()->Add(Emit::decode(val));
        };
        IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
      }
    });

    start += runlen;
  }
}

// Copy raw column bytes into a Vector.f32's repeated float buffer.  memcpy
// sidesteps alignment of the source bytes (column storage is byte-packed
// and not guaranteed 4-byte aligned).
inline void fillVectorF32(solux::proto::Vector& vec, std::string_view bytes) {
  assert(bytes.size() % sizeof(float) == 0);
  int32_t nFloats = (int32_t)(bytes.size() / sizeof(float));
  auto& fs = *vec.mutable_f32()->mutable_v();
  fs.Resize(nFloats, 0.0f);
  std::memcpy(fs.mutable_data(), bytes.data(), bytes.size());
}

inline void loadVectorColForSegmentSingle(SearchRequest& req, std::string_view field,
                                          std::span<uint8_t> idxSpan,
                                          std::span<const segdoc> segDocs,
                                          std::span<solux::proto::Vector*> target)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(poolGuard.pool(), postingsReader);
  if (!fieldReader.seek(field)) return;
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);

  auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes) {
    assert(segDocs[idxSpan[idx]].docId() == doc);
    unused(doc);
    fillVectorF32(*target[idxSpan[idx]], bytes);
  };
  StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
}

inline void loadVectorColForSegmentMulti(SearchRequest& req, std::string_view field,
                                         std::span<uint8_t> idxSpan,
                                         std::span<const segdoc> segDocs,
                                         std::span<solux::proto::ArrVector*> target)
{
  auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
  auto segNum = sortedSegDocs[0].segment();
  auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

  auto& postingsReader = req.reader->segments()[segNum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  FieldReader fieldReader(poolGuard.pool(), postingsReader);
  if (!fieldReader.seek(field)) return;
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);

  auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes, int64_t valIdx, int64_t numVals) {
    assert(segDocs[idxSpan[idx]].docId() == doc);
    unused(doc);
    auto* outer = target[idxSpan[idx]]->mutable_v();
    if (valIdx == 0) outer->Reserve(numVals);
    fillVectorF32(*outer->Add(), bytes);
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
                          SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
{
  auto columnSize = segDocs.size();
  if (columnSize == 0) return;
  auto& fieldCol = columnsProto[field];

  if (!fieldType.multiValued()) {
    auto& vecsProto = *fieldCol.mutable_col_vec()->mutable_v();
    vecsProto.Reserve(columnSize);
    for (auto i = 0u; i < columnSize; i++) vecsProto.Add();
    std::span<solux::proto::Vector*> target{vecsProto.mutable_data(), columnSize};

    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [idxSpan, &req, field, segDocs, target]() {
        loadVectorColForSegmentSingle(req, field, idxSpan, segDocs, target);
      });
      start += runlen;
    }
  } else {
    auto& arrVecsProto = *fieldCol.mutable_multi_vec()->mutable_v();
    arrVecsProto.Reserve(columnSize);
    for (auto i = 0u; i < columnSize; i++) arrVecsProto.Add();
    std::span<solux::proto::ArrVector*> target{arrVecsProto.mutable_data(), columnSize};

    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [idxSpan, &req, field, segDocs, target]() {
        loadVectorColForSegmentMulti(req, field, idxSpan, segDocs, target);
      });
      start += runlen;
    }
  }
}

// Copy stored values for one doc into a request's output slot.
inline void storeValuesInReq(const StoredReq& r, size_t slot, std::span<const std::string_view> values) {
  if (values.empty()) return;
  if (!r.multi) {
    *r.starget[slot] = std::string(values[0]);
    if (r.present) r.present[slot] = 1;
  } else {
    solux::proto::ArrStr& target = *r.mtarget[slot];
    target.mutable_v()->Reserve((int)values.size());
    for (auto v : values) {
      auto* strProto = target.mutable_v()->Add();
      *strProto = std::string(v);
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
                             const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg)
{
  // Move into a shared_ptr so every segment task can safely reference the
  // same vector even after this function returns.
  auto reqs = std::make_shared<std::vector<StoredReq>>(std::move(reqsOwned));

  int32_t start = 0;
  for (auto runlen : segRunLength) {
    auto idxSpan = sortedIdx.subspan(start, runlen);
    task_group_run(tg, [idxSpan, &req, resourceName, reqs, segDocs]() {
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
                             r->present);
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
            storeValuesInReq(*r, idxSpan[i], values);
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
                storeValuesInReq(*r, idxSpan[i], values);
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

// Stream a final ranked list of (segdoc, score) pairs back to the client as one
// or more SearchResponses.  Both TopDocsReq and FusionOp call this after their
// own ranking is complete.  The caller's `getDocList(response)` lambda returns
// the DocList proto in `response` to populate - for TopDocsReq it walks
// calc.getTarget(...)->mutable_docs() (mutex-protected).
//
// `getDoc(i)` and `getScore(i)` are invoked lazily, only for indices in the
// current batch - no upfront materialization of a parallel array, so callers
// can wire them directly to whatever layout they have (collector ScoreDoc /
// SortDoc spans, RRF result vectors, etc.).  `getScore` is only called when
// `getScores` is true.
//
// Blocks per batch on an internal task_group while column / stored-field loaders
// fan out across segments, then sends each batch via req.reply() except the
// last (the caller is expected to send req.lastResponse afterward).  All but
// the final response have set_more(true).
template <typename GetDocList, typename GetDoc, typename GetScore>
void emitDocsResponse(SearchRequest& req,
                      GetDocList&& getDocList,
                      int64_t numCollected,
                      GetDoc&& getDoc,
                      GetScore&& getScore,
                      int64_t totalHits,
                      const google::protobuf::RepeatedPtrField<std::string>& fields,
                      int32_t batchSize,
                      int64_t offset,
                      bool getNumber,
                      bool getScores)
{
  int32_t maxBatchSize = batchSize;
  if (maxBatchSize <= 0) {
    maxBatchSize = 100;  // what should the default be?
  } else if (maxBatchSize > 256) {
    maxBatchSize = 256;
  }

  // If no documents were collected, we still need to send an empty response
  int64_t totalBatches = numCollected > 0 ? numCollected : 1;
  for (int64_t batchStart = 0; batchStart < totalBatches; batchStart += maxBatchSize) {
    int64_t batchEnd = std::min(batchStart + maxBatchSize, numCollected);
    int64_t batchSizeLocal = batchEnd - batchStart;

    bool lastResponse = batchEnd >= numCollected;
    auto& response = lastResponse ? *req.lastResponse : *SearchResponse::create(req, lastResponse);
    auto& docListProto = getDocList(&response.proto);
    docListProto.set_offset(offset + batchStart);
    if (!lastResponse) {
      docListProto.set_more(true);
      response.proto.set_more(true);  // also set at the response level for easier client handling.
    }

    if (getNumber) {
      docListProto.set_matches(totalHits);
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

    std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
    oneapi::tbb::task_group* tg = req.tg ? &loadColumnsTaskGroup.emplace() : nullptr;

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

    auto& columnsProto = *docListProto.mutable_columns();

    // Single-valued columns register here so the post-wait finish pass can
    // pick each column's exact missing_val and fill the missing slots.
    PendingCols pendingCols;

    // Stored-field retrieval is grouped by resource so that multiple stored
    // fields sharing one resource decompress each chunk only once.
    boost::unordered_flat_map<std::string_view, std::vector<StoredReq>,
                              PackedTermHash, PackedTermEqual> storedByResource;

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
        std::span<std::string*> starget;
        std::span<solux::proto::ArrStr*> mtarget;
        uint8_t* present = allocStringColumn(columnsProto[field], columnSize, fieldType.multiValued(),
                                             starget, mtarget, pendingCols);
        storedByResource[resourceName].push_back(
            {field, &fieldType, fieldType.multiValued(), starget, mtarget, present});
      };

      switch (fieldType.type()) {
        case FieldType::Type::INT: {
          loadNumCol<IntColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg, pendingCols);
          break;
        }
        case FieldType::Type::FLOAT: {
          loadNumCol<FloatColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg, pendingCols);
          break;
        }
        case FieldType::Type::DOUBLE: {
          loadNumCol<DoubleColEmit>(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg, pendingCols);
          break;
        }
        case FieldType::Type::ID:
        case FieldType::Type::STRING: {
          if (fieldType.isStored()) {
            recordStoredReq(fieldType.storedResource_);
          } else if (fieldType.hasColumn()) {
            loadStrCol(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg, pendingCols);
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
          loadVectorCol(req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
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
                         segDocs, sortedIdx, segRunLength, tg);
      } else {
        for (auto& r : reqs) {
          loadStrColWithTargets(req, r.fieldName, *r.fieldType, r.starget, r.mtarget, r.present,
                                segDocs, sortedIdx, segRunLength, tg);
        }
      }
    }

    if (returnScores) {
      auto& scoresProto = columnsProto["_score_"];
      auto& floatColProto = *scoresProto.mutable_col_f();
      auto& floatsProto = *floatColProto.mutable_v();
      floatsProto.Reserve(columnSize);
      for (int i = 0; i < columnSize; i++) {
        floatsProto.Add(getScore(batchStart + i));
      }
    }

    if (tg != nullptr) {
      tg->wait();
    }

    // All slots are loaded; pick each single-valued column's exact
    // missing_val and fill its missing slots.
    finishPendingCols(pendingCols);

    if (!lastResponse) {
      auto numBuffered = req.reply(response);
      unused(numBuffered);
    }
  }
}

} // namespace solux
