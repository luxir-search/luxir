// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "luxir/reader/BKDReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/search/FieldComparator.h"
#include "luxir/util/geo.h"
#include "luxir/util/screaming.h"

namespace luxir {

// Where a geo distance sort measures from, and which point of a
// multi-valued doc it ranks by.
struct GeoSortOrigin {
  double lat;
  double lon;
  // Rank a multi-valued doc by its farthest point (max) instead of its
  // nearest (min). Single-valued fields ignore it.
  bool farthest = false;
};

// Sorts by great-circle distance from an origin over a GEO_POINT column:
// the value of geo_distance(field, lat, lon), or of min()/max() over it for
// a multi-valued field. Slots hold rank keys, smaller is better: the
// haversine sort key ascending, its negation descending, and +inf for a doc
// without a value, which sorts last either way.
//
// Two pruning aids apply to ascending nearest-point sorts once the heap is
// full. compareBottom rejects a point outside the bounding box of the circle
// at the bottom's distance before computing its distance. competitiveDocs
// walks the segment's points index for the docs inside that circle, so the
// collector can jump the query over every other doc. The set is built only
// when the points index estimates it at under an eighth of the docs the
// query still produces in the segment, and rebuilt each time the bottom
// distance halves, so the walks shrink geometrically.
class GeoDistanceComparator final : public FieldComparator {
public:
  // Benchmark seams: turn off the bounding-box test, the points-index
  // competitive set, or both.
  static inline bool disableBoxForTests = false;
  static inline bool disableIndexForTests = false;

private:
  GeoSortOrigin origin;
  bool descending;
  std::optional<IntColReader> reader;
  std::optional<IntColReader::SparseValues> pointValues;
  std::optional<screaming::BitSet::Iterator> docsIter;
  std::optional<BKDReader> bkd;
  bool multiValued = false;
  std::vector<double> values;

  // Cached rank key of the last doc read.
  int32_t lastDoc = -1;
  double lastKey = 0.0;

  // Bounding box of the circle at boxKey, in encoded coordinates. Valid
  // only for a finite ascending bottom.
  double boxKey = std::numeric_limits<double>::quiet_NaN();
  bool boxValid = false;
  int32_t boxLatMin = 0;
  int32_t boxLatMax = 0;
  int32_t boxLonMin = 0;
  int32_t boxLonMax = 0;

  // Competitive docs of the current segment: every doc >= candidateFrom with
  // a point within candidateMeters, ascending.
  std::vector<int32_t> candidates;
  size_t candidateCursor = 0;
  bool candidatesActive = false;
  // The last doc the presence iterator was asked for.
  int32_t docsTarget = -1;
  // Distance of the last circle built or priced; the next attempt waits
  // until the bottom is within half of it.
  double attemptedMeters = std::numeric_limits<double>::infinity();

  static constexpr double MISSING = std::numeric_limits<double>::infinity();

  bool nearestMode() const { return !descending && (!multiValued || !origin.farthest); }

  double pointKey(int64_t packed) const {
    return geo::haversinSortKey(
        origin.lat, origin.lon,
        geo::decodeLatitude(geo::unpackLatitude(packed)),
        geo::decodeLongitude(geo::unpackLongitude(packed)));
  }

  bool insideBox(int64_t packed) const {
    int32_t lat = geo::unpackLatitude(packed);
    return boxLatMin <= lat && lat <= boxLatMax
        && geo::longitudeInRange(geo::unpackLongitude(packed), boxLonMin,
                                 boxLonMax);
  }

  // Value ranks of `doc`, or false when it has no value.
  bool valueRanks(int32_t doc, int64_t& start, int64_t& end) {
    if (!reader.has_value()) return false;
    int32_t rank = doc;
    if (docsIter.has_value()) {
      if (doc < docsTarget) docsIter.emplace(reader->docsWithValueBitSet());
      docsTarget = doc;
      if (docsIter->val() < doc) docsIter->advance(doc);
      if (docsIter->val() != doc) return false;
      rank = docsIter->rank();
    } else if (doc >= reader->docsWithValue()) {
      return false;
    }
    if (!multiValued) {
      start = rank;
      end = rank + 1;
    } else {
      std::tie(start, end) = reader->getStartEndValueRank(rank);
    }
    return start < end;
  }

  // Rank key of `doc`. With a box, points outside it are skipped: they are
  // all farther than the bottom, so the result still orders correctly
  // against it, though it may then exceed the doc's true key.
  double docKey(int32_t doc, bool useBox) {
    if (doc == lastDoc && !useBox) return lastKey;
    int64_t start;
    int64_t end;
    if (!valueRanks(doc, start, end)) return MISSING;
    double best = MISSING;
    if (nearestMode()) {
      for (int64_t rank = start; rank < end; rank++) {
        int64_t packed = pointValues->valueAt(rank);
        if (useBox && !insideBox(packed)) continue;
        best = std::min(best, pointKey(packed));
      }
      if (useBox && best == MISSING) {
        return std::numeric_limits<double>::max();
      }
    } else {
      double key = pointKey(pointValues->valueAt(start));
      for (int64_t rank = start + 1; rank < end; rank++) {
        double point = pointKey(pointValues->valueAt(rank));
        key = origin.farthest ? std::max(key, point) : std::min(key, point);
      }
      best = descending ? -key : key;
    }
    if (!useBox) {
      lastDoc = doc;
      lastKey = best;
    }
    return best;
  }

  void updateBox(double bottomKey) {
    if (bottomKey == boxKey) return;
    boxKey = bottomKey;
    boxValid = false;
    if (!nearestMode() || !std::isfinite(bottomKey) || disableBoxForTests) return;
    double meters = geo::haversinMeters(bottomKey);
    geo::BoundingBox box = geo::circleBoundingBox(origin.lat, origin.lon, meters + 0.01);
    boxLatMin = geo::encodeLatitude(box.minLat);
    boxLatMax = geo::encodeLatitudeCeil(box.maxLat);
    boxLonMin = geo::encodeLongitude(box.minLon);
    boxLonMax = geo::encodeLongitudeCeil(box.maxLon);
    boxValid = true;
  }

  static int compareKeys(double a, double b) { return (a > b) - (a < b); }

public:
  GeoDistanceComparator(bool descending, GeoSortOrigin origin,
                        IndexReader::FieldSegments fields)
    : FieldComparator(fields), origin(origin), descending(descending) {}

  void setSegment(const IndexReader::Segment& segment) override {
    reader.reset();
    pointValues.reset();
    docsIter.reset();
    bkd.reset();
    multiValued = false;
    lastDoc = -1;
    docsTarget = -1;
    candidates.clear();
    candidateCursor = 0;
    candidatesActive = false;
    attemptedMeters = std::numeric_limits<double>::infinity();
    auto* info = fields.at(segment);
    if (info == nullptr || info->columnLoc.offset() <= 0) return;
    auto& postingsReader = segment.postingsReader();
    reader.emplace(postingsReader, *info);
    if (reader->numValues() == 0) {
      reader.reset();
      return;
    }
    pointValues.emplace(*reader);
    multiValued = reader->multiValued();
    if (!reader->denseDocsWithValue()) docsIter.emplace(reader->docsWithValueBitSet());
    if (info->pointsMetaOff != 0) bkd.emplace(postingsReader, *info);
  }

  void setBottom(int32_t slot) override {
    updateBox(values[(size_t)slot]);
  }

  LUXIR_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc,
                                   segdoc newDoc) override {
    unused(bottomDoc);
    double bottom = values[(size_t)bottomSlot];
    updateBox(bottom);
    return compareKeys(bottom, docKey(newDoc.docId(), boxValid));
  }

  int compareCurrent(int32_t slot, segdoc oldDoc, segdoc newDoc) override {
    unused(oldDoc);
    return compareKeys(values[(size_t)slot], docKey(newDoc.docId(), false));
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    return compareKeys(values[(size_t)slotA], values[(size_t)slotB]);
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB,
              segdoc docB) override {
    unused(docA, docB);
    auto& geo = static_cast<GeoDistanceComparator&>(other);
    return compareKeys(values[(size_t)slotA], geo.values[(size_t)slotB]);
  }

  void copy(int32_t slot, segdoc doc) override {
    values[(size_t)slot] = docKey(doc.docId(), false);
  }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot,
            segdoc otherDoc) override {
    unused(otherDoc);
    values[(size_t)slot] =
        static_cast<GeoDistanceComparator&>(other).values[(size_t)otherSlot];
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > values.size()) values.resize((size_t)capacity);
  }

  bool offersCompetitiveDocs() const override { return true; }

  CompetitiveDocStatus competitiveDocs(int32_t bottomSlot, int32_t from,
                                       int64_t sourceDocs, int32_t& begin,
                                       int32_t& end) override {
    if (!nearestMode() || !reader.has_value() || !bkd.has_value()
        || disableIndexForTests) {
      return CompetitiveDocStatus::UNSUPPORTED;
    }
    double bottom = values[(size_t)bottomSlot];
    if (!std::isfinite(bottom)) return CompetitiveDocStatus::PENDING;
    double meters = geo::haversinMeters(bottom);
    if (meters <= attemptedMeters * 0.5) {
      attemptedMeters = meters;
      BKDDistanceRelation relation(origin.lat, origin.lon,
                                   meters * (1.0 + 1e-9) + 0.01);
      uint64_t estimate = bkd->estimateIntersect(relation).upperBound;
      if (estimate * 8 <= (uint64_t)std::max<int64_t>(0, sourceDocs)) {
        candidates.clear();
        size_t leafSize = bkd->maxPointsPerLeaf();
        std::vector<uint32_t> docs(leafSize);
        std::vector<uint32_t> lat(leafSize);
        std::vector<uint32_t> lon(leafSize);
        auto add = [&](int32_t doc) {
          if (doc >= from) candidates.push_back(doc);
        };
        bkd->intersect(relation, {docs, lat, lon}, add,
            [&](int32_t first, int32_t last) {
              for (int32_t doc = first; doc < last; doc++) add(doc);
            },
            [&](int32_t word, uint64_t bits) {
              while (bits != 0) {
                add(word * 64 + std::countr_zero(bits));
                bits &= bits - 1;
              }
            });
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());
        candidateCursor = 0;
        candidatesActive = true;
        skipCount(SkipStats::geoSortCircleBuilds);
      }
    }
    if (!candidatesActive) return CompetitiveDocStatus::PENDING;
    // Consults ascend within a segment, so the cursor only moves forward.
    while (candidateCursor < candidates.size() && candidates[candidateCursor] < from) {
      candidateCursor++;
    }
    if (candidateCursor == candidates.size()) {
      begin = end = PostingsReader::END;
      return CompetitiveDocStatus::READY;
    }
    begin = candidates[candidateCursor];
    end = begin + 1;
    for (size_t i = candidateCursor + 1; i < candidates.size() && candidates[i] == end; i++) {
      end++;
    }
    return CompetitiveDocStatus::READY;
  }

  // Distance in meters of `doc` for this sort, or nullopt when it has no
  // value: the sort value a response reports.
  static std::optional<double> meters(IndexReader::Segment& segment,
                                      const SegFieldInfo& info,
                                      const GeoSortOrigin& origin,
                                      int32_t doc) {
    if (info.columnLoc.offset() <= 0) return std::nullopt;
    IntColReader column(segment.postingsReader(), info);
    IntColReader::Iterator iter(column);
    if (iter.advance(doc) != doc) return std::nullopt;
    auto key = [&](int64_t packed) {
      return geo::haversinSortKey(
          origin.lat, origin.lon,
          geo::decodeLatitude(geo::unpackLatitude(packed)),
          geo::decodeLongitude(geo::unpackLongitude(packed)));
    };
    if (!column.multiValued()) return geo::haversinMeters(key(iter.value()));
    auto [start, end] = column.getStartEndValueRank(iter.rank());
    double best = origin.farthest ? -1.0 : std::numeric_limits<double>::infinity();
    for (int64_t rank = start; rank < end; rank++) {
      double value = key(iter.values().valueAt(rank));
      best = origin.farthest ? std::max(best, value) : std::min(best, value);
    }
    return geo::haversinMeters(best);
  }
};

} // namespace luxir
