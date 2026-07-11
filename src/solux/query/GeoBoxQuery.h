#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "solux/query/Query.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/geo.h"

namespace solux {

// Inclusive latitude/longitude box over packed GEO_POINT column values.
// Bounds are supplied in degrees and quantized exactly like Lucene's
// LatLonPoint.newBoxQuery.
class GeoBoxQuery final : public Query {
  std::string_view field;
  int32_t minLatitude;
  int32_t maxLatitude;
  int32_t minLongitude;
  int32_t maxLongitude;
  bool empty = false;

  static bool pointInBox(int64_t packed, int32_t minLatitude,
                         int32_t maxLatitude, int32_t minLongitude,
                         int32_t maxLongitude) {
    int32_t latitude = geo::unpackLatitude(packed);
    if (latitude < minLatitude || latitude > maxLatitude) return false;
    return geo::longitudeInRange(geo::unpackLongitude(packed), minLongitude,
                                 maxLongitude);
  }

public:
  GeoBoxQuery(std::string_view field, double minLat, double maxLat,
              double minLon, double maxLon) : field(field) {
    geo::checkLatitude(minLat);
    geo::checkLatitude(maxLat);
    geo::checkLongitude(minLon);
    geo::checkLongitude(maxLon);
    if (minLat > maxLat) {
      throw std::invalid_argument("minimum latitude exceeds maximum latitude");
    }

    // +90 and the non-wrapping singleton +180 are not representable after
    // quantization. Lucene returns MatchNoDocs for the same cases.
    if (minLat == 90.0 || (minLon == 180.0 && maxLon == 180.0)) {
      empty = true;
    }
    // A wrapped interval starting at +180 is equivalent to starting at -180.
    if (minLon == 180.0 && maxLon < minLon) minLon = -180.0;

    minLatitude = geo::encodeLatitudeCeil(minLat);
    maxLatitude = geo::encodeLatitude(maxLat);
    minLongitude = geo::encodeLongitudeCeil(minLon);
    maxLongitude = geo::encodeLongitude(maxLon);

    if (maxLatitude < minLatitude) empty = true;
    if (minLon <= maxLon && maxLongitude < minLongitude) empty = true;
  }

  std::string_view getField() const { return field; }
  int32_t getMinLatitude() const { return minLatitude; }
  int32_t getMaxLatitude() const { return maxLatitude; }
  int32_t getMinLongitude() const { return minLongitude; }
  int32_t getMaxLongitude() const { return maxLongitude; }
  bool isEmpty() const { return empty; }

  Query::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<Weight>(context, *this, flags);
  }

  class BoxScorer final : public Query::Scorer {
    IntColReader& reader;
    IntColReader::SparseIterator iter;
    int32_t minLatitude;
    int32_t maxLatitude;
    int32_t minLongitude;
    int32_t maxLongitude;
    bool multi;
    int32_t docid = -1;

    bool pointMatches(int64_t packed) const {
      return pointInBox(packed, minLatitude, maxLatitude, minLongitude,
                        maxLongitude);
    }

    bool valueInBox() {
      if (docid == PostingsReader::END) return false;
      if (!multi) return pointMatches(iter.value());
      auto [start, end] = reader.getStartEndValueRank(iter.rank());
      for (int64_t rank = start; rank < end; rank++) {
        if (pointMatches(iter.values().valueAt(rank))) return true;
      }
      return false;
    }

  public:
    BoxScorer(IntColReader& reader, int32_t minLatitude,
              int32_t maxLatitude, int32_t minLongitude,
              int32_t maxLongitude)
      : reader(reader), iter(reader), minLatitude(minLatitude),
        maxLatitude(maxLatitude), minLongitude(minLongitude),
        maxLongitude(maxLongitude), multi(reader.multiValued()) {}

    bool hasTwoPhase() const override { return true; }
    int32_t approximationNext() override {
      docid = iter.next();
      return docid;
    }
    int32_t approximationAdvance(int32_t target) override {
      docid = iter.advance(target);
      return docid;
    }
    int32_t approximationDocId() override { return docid; }
    bool matches() override { return valueInBox(); }
    float matchCost() override {
      if (!multi) return 2.0f;
      int64_t docs = reader.docsWithValue();
      return docs > 0 ? 2.0f * (float)reader.numValues() / (float)docs : 2.0f;
    }

    int32_t next() override {
      for (;;) {
        docid = iter.next();
        if (docid == PostingsReader::END || valueInBox()) return docid;
      }
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      docid = iter.advance(target);
      while (docid != PostingsReader::END && !valueInBox()) {
        docid = iter.next();
      }
      return docid;
    }
    int32_t docId() override { return docid; }
    float score() override { return 0.0f; }
  };

  class Weight final : public Query::Weight {
    GeoBoxQuery& query;
    std::span<SegFieldInfo*> segInfos;

    bool segmentInfo(IndexReader::Segment& segment, SegFieldInfo*& info) const {
      if (segInfos.empty()) return false;
      info = segInfos[(size_t)segment.ord];
      return info != nullptr && info->columnLoc.offset() > 0;
    }

  public:
    Weight(Context& context, GeoBoxQuery& query, int32_t flags)
      : Query::Weight(context, flags), query(query) {
      traits |= IS_CONSTANT_SCORING;
      segInfos = context.readSegInfos(query.getField());
    }

    class Supplier final : public Query::ScorerSupplier {
      GeoBoxQuery& query;
      IntColReader& reader;

    public:
      Supplier(GeoBoxQuery& query, IntColReader& reader)
        : query(query), reader(reader) {}

      int64_t cost() override { return reader.docsWithValue(); }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return targetPool.make<BoxScorer>(
            reader, query.getMinLatitude(), query.getMaxLatitude(),
            query.getMinLongitude(), query.getMaxLongitude());
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool,
                                           IndexReader::Segment& segment) override {
      if (query.isEmpty()) return nullptr;
      SegFieldInfo* info = nullptr;
      if (!segmentInfo(segment, info)) return nullptr;
      if (info->type != FieldType::GEO_POINT) {
        throw std::runtime_error("GeoBoxQuery requires a GEO_POINT field");
      }
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *info);
      if (reader->numValues() == 0) return nullptr;
      return targetPool.make<Supplier>(query, *reader);
    }

    Query::Scorer* createScorer(MemPool& targetPool,
                                IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      return supplier == nullptr ? nullptr
          : supplier->get(targetPool, std::numeric_limits<int64_t>::max());
    }

    int64_t count(IndexReader::Segment& segment) override {
      unused(segment);
      return -1;
    }
  };
};

} // namespace solux
