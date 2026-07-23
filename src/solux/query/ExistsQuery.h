#pragma once

#include <limits>
#include <span>
#include <string_view>

#include "AllQuery.h"
#include "Query.h"
#include "solux/reader/DocsReader.h"

namespace solux {

// Matches documents that supplied at least one accepted value for a field.
// Presence is read directly from SegFieldInfo, so indexed, column-only, and
// indexed text fields with no terms share the same execution path.
class ExistsQuery final : public Query {
  std::string_view field;

public:
  explicit ExistsQuery(std::string_view field) : field(field) {}

  std::string_view getField() const { return field; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::EXISTS);
    out.appendString(field);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Weight;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override;

  class Scorer final : public Query::ConstantScorer {
    DocsReader docs;
    screaming::BitSet::Iterator iterator;

  public:
    Scorer(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo,
           float constantScore)
        : Query::ConstantScorer(constantScore),
          docs(postingsReader, fieldInfo), iterator(docs.bitset()) {
      assert(fieldInfo.docsWithField > 0);
      assert(fieldInfo.docsWithField < postingsReader.maxDoc());
      assert(docs.hasBitset());
    }

    int32_t next() override {
      return iterator.next();
    }

    int32_t advance(int32_t target) override {
      assert(docId() < target);
      return iterator.advance(target);
    }

    int32_t docId() override { return iterator.val(); }
  };

  class Weight final : public Query::Weight {
    ExistsQuery& query;
    std::span<SegFieldInfo*> segInfos;
    float constantScore;

    SegFieldInfo* segmentInfo(IndexReader::Segment& segment) const {
      return segInfos.empty() ? nullptr : segInfos[(size_t)segment.ord];
    }

  public:
    Weight(Context& context, ExistsQuery& query, int32_t flags,
           float constantScore)
        : Query::Weight(context, flags), query(query),
          segInfos(context.readSegInfos(query.getField())),
          constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
    }

    class Supplier final : public Query::ScorerSupplier {
      IndexReader::Segment& segment;
      SegFieldInfo& fieldInfo;
      float constantScore;

    public:
      Supplier(IndexReader::Segment& segment, SegFieldInfo& fieldInfo,
               float constantScore)
          : segment(segment), fieldInfo(fieldInfo),
            constantScore(constantScore) {}

      int64_t cost() override { return fieldInfo.docsWithField; }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        if (fieldInfo.docsWithField == segment.maxDoc()) {
          return targetPool.make<AllQuery::Scorer>(segment, constantScore);
        }
        return targetPool.make<ExistsQuery::Scorer>(
            segment.postingsReader(), fieldInfo, constantScore);
      }
    };

    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      SegFieldInfo* info = segmentInfo(segment);
      if (info == nullptr || info->docsWithField == 0) return nullptr;
      return targetPool.make<Supplier>(segment, *info, constantScore);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      Query::ScorerSupplier* supplier = scorerSupplier(targetPool, segment);
      return supplier == nullptr ? nullptr
          : supplier->get(targetPool, std::numeric_limits<int64_t>::max());
    }

    int64_t count(IndexReader::Segment& segment) override {
      if (segment.liveDocs() != nullptr) return -1;
      SegFieldInfo* info = segmentInfo(segment);
      return info == nullptr ? 0 : info->docsWithField;
    }
  };
};

inline ExistsQuery::Weight* ExistsQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  return context.pool.make<ExistsQuery::Weight>(
      context, *this, flags, Query::constantWhenScored(flags, multiplier));
}

static_assert(std::is_trivially_destructible_v<ExistsQuery::Weight>);
static_assert(std::is_trivially_destructible_v<ExistsQuery::Scorer>);

} // namespace solux
