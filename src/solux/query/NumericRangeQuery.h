#pragma once

#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>

#include "solux/query/Query.h"
#include "solux/reader/IntColReader.h"

namespace solux {

// Numeric range query executed by scanning the field's numeric column (there is
// no points/BKD index yet).  Every numeric field type stores an
// order-preserving encoded int64 in one shared int column (INT raw, FLOAT/DOUBLE
// Lucene sortable bits, DATE epoch millis - see NumericUtils.h / ValCoerce.cpp),
// so the query works purely in encoded int64 space: the builder coerces the
// user's endpoints with FieldType::coerceColInt64 and folds exclusive bounds
// into an inclusive [lo, hi] window (integers, so v > k  <=>  v >= k+1).  A doc
// matches iff any of its values falls in [lo, hi]; a doc with no value never
// matches.  Comparisons follow the field's encoded sortable order, so for
// FLOAT/DOUBLE -0.0 sorts below +0.0 and NaN sorts above +Inf.
//
// Two-phase iteration: the approximation walks the column's docs-with-value and
// matches() verifies the value(s) at the current doc.  In a selective
// conjunction a cheaper clause leads and this query verifies its candidates by
// random-access column reads instead of a full scan.  Constant scoring (score 0,
// filter semantics) like AllQuery; deletes/domain are applied by the collector,
// not here.
class NumericRangeQuery final : public Query {
  std::string_view field;
  int64_t lo;  // inclusive lower bound, encoded
  int64_t hi;  // inclusive upper bound, encoded
public:
  NumericRangeQuery(std::string_view field, int64_t lo, int64_t hi)
    : field(field), lo(lo), hi(hi) {}

  std::string_view getField() const { return field; }
  int64_t getLo() const { return lo; }
  int64_t getHi() const { return hi; }

  Query::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<Weight>(context, *this, flags);
  }

  // Two-phase scorer over one segment's numeric column.  ColIter selects the
  // value-decode strategy: bulk Iterator (block decode) when this scorer DRIVES
  // iteration (a full scan), SparseIterator (per-value decode) when it VERIFIES
  // a cheaper lead's candidates in a conjunction.  Both expose the same
  // DocIterator API.
  template <class ColIter>
  class RangeScorer final : public Query::Scorer {
    IntColReader& reader;
    ColIter iter;
    int64_t lo;
    int64_t hi;
    bool allMatch;  // segment column entirely within [lo,hi]: skip per-value compares
    bool multi;
    int32_t docid = -1;

    // Test the value(s) at the current approximation doc against [lo, hi].
    // Idempotent: reads column values, consumes no iterator state.
    bool valueInRange() {
      if (docid == PostingsReader::END) return false;
      if (!multi) {
        // A single-valued doc-with-value always has exactly one value.
        if (allMatch) return true;
        int64_t v = iter.value();
        return lo <= v && v <= hi;
      }
      // Multi-valued: match if ANY value is in range.  Empty arrays are stored
      // as docs-with-value with an empty rank range, so they must not match even
      // under allMatch (hence the start < end guard).
      auto [start, end] = reader.getStartEndValueRank(iter.rank());
      if (allMatch) return start < end;
      for (int64_t r = start; r < end; r++) {
        int64_t v = iter.values().valueAt(r);
        if (lo <= v && v <= hi) return true;
      }
      return false;
    }

  public:
    RangeScorer(IntColReader& reader, int64_t lo, int64_t hi, bool allMatch)
      : reader(reader), iter(reader), lo(lo), hi(hi), allMatch(allMatch),
        multi(reader.multiValued()) {}

    // ---- two-phase iteration ----
    bool hasTwoPhase() const override { return true; }
    int32_t approximationNext() override { docid = iter.next(); return docid; }
    int32_t approximationAdvance(int32_t target) override { docid = iter.advance(target); return docid; }
    int32_t approximationDocId() override { return docid; }
    bool matches() override { return valueInRange(); }
    float matchCost() override {
      if (allMatch) return 0.0f;
      if (!multi) return 1.0f;  // one column read + compare
      int64_t docs = reader.docsWithValue();
      return docs > 0 ? (float)reader.numValues() / (float)docs : 1.0f;
    }

    // ---- single-phase (standalone / non-two-phase consumers) ----
    int32_t next() override {
      for (;;) {
        docid = iter.next();
        if (docid == PostingsReader::END || valueInRange()) return docid;
      }
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      docid = iter.advance(target);
      while (docid != PostingsReader::END && !valueInRange()) {
        docid = iter.next();
      }
      return docid;
    }
    int32_t docId() override { return docid; }
    float score() override { return 0.0f; }
  };

  class Weight final : public Query::Weight {
    NumericRangeQuery& query;
    std::span<SegFieldInfo*> segInfos;  // per-segment; null entry = field absent here
  public:
    Weight(Context& context, NumericRangeQuery& query, int32_t flags)
        : Query::Weight(context, flags), query(query) {
      traits |= IS_CONSTANT_SCORING;  // every match scores 0
      // Column-only path: read per-segment field info directly.  getCachedFieldInfo
      // would eagerly build term enums a column-stored numeric field doesn't have.
      segInfos = context.readSegInfos(query.getField());
    }

    // sparse=true builds a per-value-decode scorer (for verifying a cheaper
    // lead's candidates); false builds a block-decode scorer (for driving a scan).
    Query::Scorer* buildScorer(MemPool& targetPool, IndexReader::Segment& segment, bool sparse) {
      if (segInfos.empty()) return nullptr;
      SegFieldInfo* segInfo = segInfos[segment.ord];
      if (segInfo == nullptr || segInfo->columnLoc.offset() <= 0) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;  // no real values (e.g. all-empty arrays)
      int64_t colMin = reader->getMin();
      int64_t colMax = reader->getMax();
      if (colMax < query.getLo() || query.getHi() < colMin) return nullptr;  // disjoint
      bool allMatch = (query.getLo() <= colMin && colMax <= query.getHi());  // subset
      if (sparse) {
        return targetPool.make<RangeScorer<IntColReader::SparseIterator>>(
            *reader, query.getLo(), query.getHi(), allMatch);
      }
      return targetPool.make<RangeScorer<IntColReader::Iterator>>(
          *reader, query.getLo(), query.getHi(), allMatch);
    }

    // Default SegmentSource path drives the scorer directly -> block iterator.
    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildScorer(targetPool, segment, /*sparse=*/false);
    }

    class Supplier final : public Query::ScorerSupplier {
      NumericRangeQuery::Weight& weight;
      IndexReader::Segment& segment;
    public:
      Supplier(NumericRangeQuery::Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        if (weight.segInfos.empty()) return 0;
        SegFieldInfo* segInfo = weight.segInfos[segment.ord];
        return segInfo == nullptr ? 0 : segInfo->docsWithField;
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        // A lead cheaper than this clause's cardinality means we verify its
        // candidates (sparse random access); otherwise we drive (dense scan).
        bool sparse = leadCost < cost();
        return weight.buildScorer(targetPool, segment, sparse);
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }
  };
};

} // namespace solux
