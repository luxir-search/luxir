#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#include <boost/sort/spreadsort/integer_sort.hpp>

#include "solux/query/PointsMaterialize.h"
#include "solux/query/Query.h"
#include "solux/reader/BKDReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/geo.h"

namespace solux {

template <class Relation, class ColIter>
class GeoQueryScorer final : public Query::Scorer {
  IntColReader& reader;
  ColIter iter;
  Relation relation;
  bool multi;
  int32_t docid = -1;

  bool pointMatches(int64_t packed) const {
    return relation.matches(geo::unpackLatitude(packed),
                            geo::unpackLongitude(packed));
  }

  bool valueMatches() {
    if (docid == PostingsReader::END) return false;
    if (!multi) return pointMatches(iter.value());
    auto [start, end] = reader.getStartEndValueRank(iter.rank());
    for (int64_t rank = start; rank < end; rank++) {
      if (pointMatches(iter.values().valueAt(rank))) return true;
    }
    return false;
  }

public:
  GeoQueryScorer(IntColReader& reader, const Relation& relation)
      : reader(reader), iter(reader), relation(relation),
        multi(reader.multiValued()) {}

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
  bool matches() override { return valueMatches(); }
  float matchCost() override {
    if (!multi) return 2.0f;
    int64_t docs = reader.docsWithValue();
    return docs > 0 ? 2.0f * (float)reader.numValues() / (float)docs : 2.0f;
  }

  int32_t next() override {
    for (;;) {
      docid = iter.next();
      if (docid == PostingsReader::END || valueMatches()) return docid;
    }
  }
  int32_t advance(int32_t target) override {
    assert(docid < target);
    docid = iter.advance(target);
    while (docid != PostingsReader::END && !valueMatches()) {
      docid = iter.next();
    }
    return docid;
  }
  int32_t docId() override { return docid; }
  float score() override { return 0.0f; }
};

template <class QueryType, class Relation>
class GeoQueryWeight final : public Query::Weight {
  QueryType& query;
  std::span<SegFieldInfo*> segInfos;

  bool segmentInfo(IndexReader::Segment& segment, SegFieldInfo*& info) const {
    if (segInfos.empty()) return false;
    info = segInfos[(size_t)segment.ord];
    return info != nullptr && info->columnLoc.offset() > 0;
  }

  [[noreturn]] static void wrongFieldType() {
    throw std::runtime_error(std::string(QueryType::QUERY_NAME)
                             + " requires a GEO_POINT field");
  }

  [[noreturn]] static void countMismatch() {
    throw std::runtime_error(std::string(QueryType::QUERY_NAME)
                             + ": points/column value count mismatch");
  }

public:
  GeoQueryWeight(Query::Context& context, QueryType& query, int32_t flags)
      : Query::Weight(context, flags), query(query) {
    traits |= IS_CONSTANT_SCORING;
    segInfos = context.readSegInfos(query.getField());
  }

  class Supplier final : public Query::ScorerSupplier {
    GeoQueryWeight& weight;
    IndexReader::Segment& segment;
    IntColReader& reader;
    BKDReader* bkd;
    Relation relation;
    int64_t estimatedCost;
    uint64_t materializeUpperBound;

    BKDReader::Scratch scratch(MemPool& pool) const {
      size_t size = bkd->maxPointsPerLeaf();
      return {pool.make_span<uint32_t>(size),
              pool.make_span<uint32_t>(size),
              pool.make_span<uint32_t>(size)};
    }

    PointsMaterialize::Materialized materialize(MemPool& pool) const {
      assert(bkd != nullptr);
      BKDReader::Scratch leafScratch = scratch(pool);
      if (PointsMaterialize::useBitset((uint64_t)estimatedCost,
                                       segment.maxDoc())) {
        auto words = pool.make_span<uint64_t>(
            FixedBitSet::sizeInWords(segment.maxDoc()));
        std::fill(words.begin(), words.end(), 0);
        FixedBitSet bits(words.data(), segment.maxDoc());
        bkd->intersect(relation, leafScratch,
            [&bits](int32_t doc) { bits.set(doc); },
            [&bits](int32_t begin, int32_t end) {
              PointsMaterialize::setRun(bits, begin, end);
            },
            [&bits](int32_t wordIndex, uint64_t word) {
              bits.words[wordIndex] |= word;
            });
        return {{}, words.data()};
      }

      // The estimate half-charges crossing leaves and cannot safely size the
      // array. upperBound charges every crossing point and is emission-safe.
      auto docs = pool.make_span<int32_t>((size_t)materializeUpperBound);
      size_t size = 0;
      auto addDoc = [&docs, &size](int32_t doc) { docs[size++] = doc; };
      bkd->intersect(relation, leafScratch, addDoc,
          [&addDoc](int32_t begin, int32_t end) {
            for (int32_t doc = begin; doc < end; doc++) addDoc(doc);
          },
          [&addDoc](int32_t wordIndex, uint64_t word) {
            while (word != 0) {
              addDoc(wordIndex * 64 + (int32_t)std::countr_zero(word));
              word &= word - 1;
            }
          });
      assert(size <= materializeUpperBound);
      auto used = docs.first(size);
      // BKD output is sorted only within leaves.
      boost::sort::spreadsort::integer_sort(used.begin(), used.end());
      if (reader.multiValued()) {
        size = (size_t)(std::unique(used.begin(), used.end()) - used.begin());
      }
      return {docs.first(size), nullptr};
    }

  public:
    Supplier(GeoQueryWeight& weight, IndexReader::Segment& segment,
             IntColReader& reader, BKDReader* bkd,
             BKDReader::EstimateResult estimate)
        : weight(weight), segment(segment), reader(reader), bkd(bkd),
          relation(weight.query.makeRelation()),
          estimatedCost((int64_t)std::min<uint64_t>(
              estimate.estimatedCount, (uint64_t)reader.docsWithValue())),
          materializeUpperBound(estimate.upperBound) {}

    int64_t cost() override { return estimatedCost; }

    Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
      if (leadCost < cost()) {
        skipCount(SkipStats::geoSparseVerifyArms);
        return targetPool.make<
            GeoQueryScorer<Relation, IntColReader::SparseIterator>>(
                reader, relation);
      }
      if (bkd != nullptr) {
        skipCount(SkipStats::geoBKDArms);
        return PointsMaterialize::scorerFor(
            targetPool, materialize(targetPool), segment.maxDoc());
      }
      skipCount(SkipStats::geoScanArms);
      return targetPool.make<GeoQueryScorer<Relation, IntColReader::Iterator>>(
          reader, relation);
    }

    BulkScorer* bulkScorer(MemPool& targetPool) override {
      if (bkd == nullptr) return nullptr;
      skipCount(SkipStats::geoBKDArms);
      return PointsMaterialize::bulkFor(
          targetPool, materialize(targetPool), segment.maxDoc());
    }
  };

  Query::ScorerSupplier* scorerSupplier(
      MemPool& targetPool, IndexReader::Segment& segment) override {
    if (query.isEmpty()) return nullptr;
    SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return nullptr;
    if (info->type != FieldType::GEO_POINT) wrongFieldType();
    auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *info);
    if (reader->numValues() == 0) return nullptr;
    BKDReader* bkd = nullptr;
    BKDReader::EstimateResult estimate{
        (uint64_t)reader->docsWithValue(), (uint64_t)reader->numValues()};
    if (info->pointsMetaOff != 0) {
      bkd = targetPool.make<BKDReader>(segment.postingsReader(), *info);
      if (bkd->pointCount() != (uint64_t)reader->numValues()) countMismatch();
      Relation relation = query.makeRelation();
      estimate = bkd->estimateIntersect(relation);
      if (estimate.upperBound == 0) return nullptr;
    }
    return targetPool.make<Supplier>(*this, segment, *reader, bkd, estimate);
  }

  Query::Scorer* createScorer(MemPool& targetPool,
                              IndexReader::Segment& segment) override {
    auto* supplier = scorerSupplier(targetPool, segment);
    return supplier == nullptr ? nullptr
        : supplier->get(targetPool, std::numeric_limits<int64_t>::max());
  }

  // Test and benchmark baseline that bypasses the BKD.
  Query::Scorer* createScanScorerForTests(
      MemPool& targetPool, IndexReader::Segment& segment) {
    if (query.isEmpty()) return nullptr;
    SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return nullptr;
    if (info->type != FieldType::GEO_POINT) wrongFieldType();
    auto* reader = targetPool.make<IntColReader>(segment.postingsReader(),
                                                 *info);
    if (reader->numValues() == 0) return nullptr;
    Relation relation = query.makeRelation();
    return targetPool.make<GeoQueryScorer<Relation, IntColReader::Iterator>>(
        *reader, relation);
  }

  int64_t count(IndexReader::Segment& segment) override {
    if (query.isEmpty()) return 0;
    if (segment.liveDocs() != nullptr) return -1;
    SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return 0;
    if (info->type != FieldType::GEO_POINT) wrongFieldType();
    IntColReader reader(segment.postingsReader(), *info);
    if (reader.numValues() == 0) return 0;
    if (reader.multiValued() || info->pointsMetaOff == 0) return -1;
    BKDReader bkd(segment.postingsReader(), *info);
    if (bkd.pointCount() != (uint64_t)reader.numValues()) countMismatch();
    auto guard = MemPool::threadLocalPoolGuard();
    size_t size = bkd.maxPointsPerLeaf();
    BKDReader::Scratch scratch{{}, guard.pool().make_span<uint32_t>(size),
                               guard.pool().make_span<uint32_t>(size)};
    Relation relation = query.makeRelation();
    return (int64_t)bkd.countIntersect(relation, scratch).exactCount;
  }
};

} // namespace solux
