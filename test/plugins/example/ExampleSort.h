// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "luxir/query/FieldResolver.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/search/SortKindRegistry.h"

namespace example {

// What the abs_diff prune did, for the tests.
struct AbsDiffStats {
  // Docs competitiveDocs proved could not enter the heap.
  static inline std::atomic<int64_t> skippedDocs{0};
  // Candidate sets read from a points index.
  static inline std::atomic<int64_t> builds{0};
  // Test-only: what competitiveDocs answers instead of its own answer.
  // MALFORMED is a READY run that starts before `from` and holds no doc;
  // PENDING_AT names a doc to be asked again at.
  enum class Answer { OWN, MALFORMED, PENDING, PENDING_AT, UNSUPPORTED };
  static inline std::atomic<Answer> answer{Answer::OWN};
  // Segments seedWindows answered with windows in round 0, the later rounds
  // it answered, and the windows it offered in all rounds.
  static inline std::atomic<int64_t> seededSegments{0};
  static inline std::atomic<int64_t> seedRounds{0};
  static inline std::atomic<int64_t> seedWindows{0};
  // Test-only: what seedWindows answers. NONE offers none; MALFORMED adds
  // unsorted, overlapping, empty and out-of-range windows to its own;
  // OVERLAP offers, in a later round, the earlier rounds' leaves again with
  // the new ring.
  enum class SeedAnswer { OWN, NONE, MALFORMED, OVERLAP };
  static inline std::atomic<SeedAnswer> seedAnswer{SeedAnswer::OWN};
};

// |x - center| of one segment's INT column: the value under SINGLE, the
// nearest of a multi-valued doc's values under MIN, the farthest under MAX.
class AbsDiffColumn {
  int64_t center;
  luxir::SortKindMode mode;
  std::optional<luxir::IntColReader> column;
  std::optional<luxir::IntColReader::SparseIterator> iter;

  int64_t distance(int64_t value) const {
    __int128 d = (__int128)value - center;
    if (d < 0) d = -d;
    return d > std::numeric_limits<int64_t>::max() ? std::numeric_limits<int64_t>::max() : (int64_t)d;
  }

public:
  AbsDiffColumn(int64_t center, luxir::SortKindMode mode) : center(center), mode(mode) {}

  void setSegment(const luxir::IndexReader::Segment& segment, const luxir::SegFieldInfo* info) {
    iter.reset();
    column.reset();
    if (info == nullptr || info->columnLoc.offset() == 0) return;
    column.emplace(segment.postingsReader(), *info);
    iter.emplace(*column);
  }

  bool read(int32_t doc, int64_t& out) {
    if (!iter) return false;
    if (iter->docId() >= doc) iter->reset();
    if (iter->advance(doc) != doc) return false;
    if (!column->multiValued()) {
      out = distance(iter->value());
      return true;
    }
    auto [start, end] = column->getStartEndValueRank(iter->rank());
    out = distance(iter->values().valueAt(start));
    for (int64_t rank = start + 1; rank < end; rank++) {
      int64_t next = distance(iter->values().valueAt(rank));
      out = mode == luxir::SortKindMode::MAX ? std::max(out, next) : std::min(out, next);
    }
    return true;
  }
};

// Ranks an INT field by |x - center|. An ascending sort on the nearest value
// knows where its competitive docs are: within the bottom's distance of the
// center, which the field's points index finds. It offers them through
// competitiveDocs once they are few against the docs the query would still
// produce, and reads them again each time the bottom distance halves. Before
// the sweep it offers the docs of the points nearest the center as seed
// windows, so the bottom is near its final distance from the first consult
// whatever the doc order.
class AbsDiffComparator : public luxir::ValueSortComparator<int64_t> {
  int64_t center;
  bool prunes;
  AbsDiffColumn column;
  std::optional<luxir::PointsReader> points;
  int32_t maxDoc = 0;
  // The segment's docs within builtRadius of the center, ascending.
  std::vector<int32_t> candidates;
  int64_t builtRadius = -1;
  std::vector<uint32_t> residualScratch;
  std::vector<int64_t> rawScratch;
  // The points index leaves seedWindows offered in this segment so far.
  int64_t seedFirstLeaf = 0;
  int64_t seedPastLeaf = 0;

  static int64_t saturatingAdd(int64_t a, int64_t b) {
    __int128 sum = (__int128)a + b;
    return (int64_t)std::clamp<__int128>(sum, std::numeric_limits<int64_t>::min(),
                                         std::numeric_limits<int64_t>::max());
  }

protected:
  bool read(int32_t doc, int64_t& out) override { return column.read(doc, out); }

public:
  AbsDiffComparator(luxir::IndexReader::FieldSegments fields, int64_t center,
                    luxir::SortKindMode mode, bool descending)
      : ValueSortComparator(fields, descending), center(center),
        prunes(!descending && mode != luxir::SortKindMode::MAX), column(center, mode) {}

  void setSegment(const luxir::IndexReader::Segment& segment) override {
    const auto* info = fields.at(segment);
    column.setSegment(segment, info);
    points.reset();
    candidates.clear();
    builtRadius = -1;
    seedFirstLeaf = seedPastLeaf = 0;
    maxDoc = segment.maxDoc();
    if (prunes && info != nullptr && info->pointsMetaOff != 0) {
      points.emplace(segment.postingsReader(), *info);
      residualScratch.resize(points->maxPointsPerLeaf());
      rawScratch.resize(points->maxPointsPerLeaf());
    }
  }

  bool offersCompetitiveDocs() const override { return prunes; }

  bool offersSeedWindows() const override { return points.has_value(); }

  // The docs of the points index leaves holding the points nearest the
  // center: in round 0, enough points for the heap's matches at the query's
  // density. When those left the heap short (the query is sparser near the
  // center than overall), each later round widens the leaves offered so far
  // by matches / collected, at least doubling them, and offers the new ring.
  void seedWindows(const SeedRequest& request, std::vector<luxir::SeedWindow>& out) override {
    if (!points || AbsDiffStats::seedAnswer.load() == AbsDiffStats::SeedAnswer::NONE) return;
    int64_t perLeaf = points->maxPointsPerLeaf();
    int64_t leaves = points->leafCount();
    int64_t firstLeaf = 0;
    int64_t pastLeaf = 0;
    if (request.round == 0) {
      int64_t count = (int64_t)points->pointCount();
      int64_t density = std::max<int64_t>(1, request.sourceDocs);
      int64_t want = (int64_t)std::min<__int128>(
          count, std::max<__int128>(1, (__int128)request.matches * maxDoc / density));
      int64_t mid = (int64_t)points->ordinalOfFirstAtLeast(center, residualScratch, rawScratch);
      // The `want` points nearest the center lie within `want` ordinals of mid.
      int64_t lo = std::max<int64_t>(0, mid - want);
      int64_t hi = std::min(count, mid + want);
      if (lo >= hi) return;
      firstLeaf = lo / perLeaf;
      pastLeaf = (hi - 1) / perLeaf + 1;
    } else {
      int64_t width = seedPastLeaf - seedFirstLeaf;
      int64_t grown = (int64_t)std::min<__int128>(
          leaves, std::max<__int128>(2 * (__int128)width,
                                     (__int128)width * request.matches / std::max<int64_t>(1, request.collected)));
      // Split the growth around the leaves offered so far, the rest of it to
      // one side where the other ends.
      int64_t add = grown - width;
      int64_t left = std::min(seedFirstLeaf, add / 2);
      int64_t right = std::min(leaves - seedPastLeaf, add - left);
      left = std::min(seedFirstLeaf, add - right);
      firstLeaf = seedFirstLeaf - left;
      pastLeaf = seedPastLeaf + right;
      if (left == 0 && right == 0) return;
    }
    std::vector<int32_t> docs;
    auto addLeaves = [&](int64_t from, int64_t past) {
      for (int64_t leaf = from; leaf < past; leaf++) {
        for (const auto& point : points->decodeLeaf((uint32_t)leaf)) docs.push_back(point.docid);
      }
    };
    if (request.round == 0 || AbsDiffStats::seedAnswer.load() == AbsDiffStats::SeedAnswer::OVERLAP) {
      addLeaves(firstLeaf, pastLeaf);
    } else {
      addLeaves(firstLeaf, seedFirstLeaf);
      addLeaves(seedPastLeaf, pastLeaf);
    }
    seedFirstLeaf = firstLeaf;
    seedPastLeaf = pastLeaf;
    std::sort(docs.begin(), docs.end());
    docs.erase(std::unique(docs.begin(), docs.end()), docs.end());
    size_t first = out.size();
    for (int32_t doc : docs) {
      if (out.size() > first && out.back().end == doc) out.back().end++;
      else out.push_back({doc, doc + 1});
    }
    if (AbsDiffStats::seedAnswer.load() == AbsDiffStats::SeedAnswer::MALFORMED) {
      std::reverse(out.begin(), out.end());
      size_t own = out.size();
      for (size_t i = 0; i < own; i++) out.push_back({out[i].begin - 2, out[i].end + 1});
      out.push_back({-5, 3});
      out.push_back({maxDoc - 2, maxDoc + 100});
      out.push_back({std::numeric_limits<int32_t>::max() - 1, std::numeric_limits<int32_t>::max()});
      out.push_back({7, 7});
      out.push_back({20, 10});
    }
    if (request.round == 0) AbsDiffStats::seededSegments++;
    else AbsDiffStats::seedRounds++;
    AbsDiffStats::seedWindows += (int64_t)out.size();
  }

  CompetitiveDocStatus competitiveDocs(int32_t bottomSlot, int32_t from, int64_t sourceDocs,
                                       int32_t& begin, int32_t& end) override {
    if (!points) return CompetitiveDocStatus::UNSUPPORTED;
    switch (AbsDiffStats::answer.load()) {
      case AbsDiffStats::Answer::OWN: break;
      case AbsDiffStats::Answer::MALFORMED:
        begin = end = std::max(0, from - 3);
        return CompetitiveDocStatus::READY;
      case AbsDiffStats::Answer::PENDING: return CompetitiveDocStatus::PENDING;
      case AbsDiffStats::Answer::PENDING_AT:
        end = std::min(from + 7, maxDoc);
        return CompetitiveDocStatus::PENDING;
      case AbsDiffStats::Answer::UNSUPPORTED: return CompetitiveDocStatus::UNSUPPORTED;
    }
    const Key& bottom = slot(bottomSlot);
    if (!bottom.present) return CompetitiveDocStatus::PENDING;
    int64_t radius = bottom.value;
    if (builtRadius < 0 || (radius < builtRadius && radius <= builtRadius / 2)) {
      int64_t lo = saturatingAdd(center, -radius);
      int64_t hi = saturatingAdd(center, radius);
      // The points are sorted by value, so their ordinals count the range.
      uint64_t first = points->ordinalOfFirstAtLeast(lo, residualScratch, rawScratch);
      uint64_t past = hi == std::numeric_limits<int64_t>::max()
          ? points->pointCount() : points->ordinalOfFirstAtLeast(hi + 1, residualScratch, rawScratch);
      if ((int64_t)(past - first) * 8 <= sourceDocs) {
        candidates.clear();
        points->forEachRangeDoc(lo, hi, [&](int32_t doc) { candidates.push_back(doc); });
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        builtRadius = radius;
        AbsDiffStats::builds++;
      } else if (builtRadius < 0) {
        // Ask again once a few hundred more docs have tightened the bottom.
        end = (int32_t)std::min<int64_t>((int64_t)from + 256, maxDoc);
        return CompetitiveDocStatus::PENDING;
      }
    }
    auto it = std::lower_bound(candidates.begin(), candidates.end(), from);
    if (it == candidates.end()) {
      AbsDiffStats::skippedDocs += std::max(0, maxDoc - from);
      begin = end = luxir::PostingsReader::END;
      return CompetitiveDocStatus::READY;
    }
    AbsDiffStats::skippedDocs += *it - from;
    begin = *it;
    end = begin + 1;
    while (++it != candidates.end() && *it == end) end++;
    return CompetitiveDocStatus::READY;
  }
};

class AbsDiffSource : public luxir::SortKindSource {
  std::string field;
  int64_t center;
  luxir::SortKindMode mode;
  bool descending;

public:
  AbsDiffSource(std::string field, int64_t center, luxir::SortKindMode mode, bool descending)
      : field(std::move(field)), center(center), mode(mode), descending(descending) {}

  std::unique_ptr<luxir::FieldComparator> createComparator(luxir::IndexReader& reader) const override {
    return std::make_unique<AbsDiffComparator>(reader.field(field), center, mode, descending);
  }

  void value(luxir::IndexReader& reader, const luxir::IndexReader::Segment& segment, int32_t doc,
             luxir::api::Val& out, std::pmr::memory_resource&) const override {
    AbsDiffColumn column(center, mode);
    column.setSegment(segment, reader.field(field).at(segment));
    int64_t distance;
    if (column.read(doc, distance)) out.kind = distance;
  }
};

// abs_diff(field, center): sorts an INT field by its distance from an integer
// (a literal or an integer $variable); min() and max() select the nearest or
// farthest value of a multi-valued field.
inline std::shared_ptr<const luxir::SortKindSource> makeAbsDiff(luxir::SortKindContext& ctx) {
  ctx.arity(2, 2);
  const auto& field = ctx.field(0);
  auto& type = *field.fieldType;
  if (type.type() != luxir::FieldType::INT || !type.hasColumn()) {
    ctx.fail(fmt::format("abs_diff() field '{}' must be an INT field with column values", field.logicalName));
  }
  if (type.multiValued() && ctx.mode == luxir::SortKindMode::SINGLE) {
    ctx.fail(fmt::format("abs_diff() over multi-valued field '{}' needs min() or max()", field.logicalName));
  }
  int64_t center = ctx.integer(1);
  ctx.note(fmt::format("sort: abs_diff({}, {})", field.physicalName, center));
  return std::make_shared<AbsDiffSource>(field.physicalName, center, ctx.mode, ctx.descending);
}

}  // namespace example
