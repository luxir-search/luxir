#pragma once

#include <cstring>
#include <string_view>

#include "Query.h"
#include "solux/reader/FilteredTermsEnum.h"
#include "solux/util/screaming.h"

namespace solux {

// Base for constant-score queries that union postings from multiple terms in
// one field. Subclasses provide the filtered term iterator.
class MultiTermQuery : public Query {
protected:
  std::string_view field;
  float boost;

public:
  MultiTermQuery(std::string_view field, float boost = 1.0f) : field(field), boost(boost) {}

  std::string_view getField() const { return field; }
  float getBoost() const { return boost; }

  // Build the per-segment filtered term iterator.
  virtual FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) = 0;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<MultiTermQuery::Weight>(context, *this, flags, multiplier);
  }

  // Iterates set bits of the membership bitset under a constant score.
  class Scorer final : public Query::Scorer {
    FixedBitSet bits;
    int32_t maxDoc;
    float boost;
    int32_t docid = -1;
    bool exhausted = false;  // latched when `boost` can no longer compete

    // First set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }

  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float boost) : bits(bits), maxDoc(maxDoc), boost(boost) {}

    int32_t next() override {
      if (exhausted || docid == PostingsReader::END) return docid = PostingsReader::END;
      return advanceTo(docid + 1);
    }
    int32_t advance(int32_t target) override {
      if (exhausted) return docid = PostingsReader::END;
      assert(docid < target);  // strict advance
      return advanceTo(target);
    }
    int32_t docId() override { return docid; }
    float score() override { return boost; }

    // Every match scores exactly `boost`: a flat, exact bound with no
    // shallow structure.  Once the collector's floor rises above it, no
    // remaining doc can compete (ties stay competitive).
    void setMinCompetitiveScore(float minScore) override {
      if (minScore > boost) exhausted = true;
    }
    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return boost;
    }
    float getMaxScoreForSetup(int32_t upTo) override {
      unused(upTo);
      return boost;
    }
    int32_t advanceShallowForSetup(int32_t target) override {
      unused(target);
      return PostingsReader::END;
    }
  };

  class Weight final : public Query::Weight {
    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;

  public:
    Weight(Context& context, MultiTermQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(checkedBoostProduct(multiplier, query.getBoost())) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (cachedFieldInfo == nullptr) return nullptr;       // field absent everywhere
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) return nullptr;          // field absent in this segment

      // Fresh per-segment cursor because scorers can be built in parallel.
      auto* termsEnum = targetPool.make<TermsEnum>(targetPool, segment.postingsReader(), *segFieldInfo);
      FilteredTermsEnum* fenum = query.createFilteredEnum(targetPool, *termsEnum);

      int32_t maxDoc = segment.postingsReader().maxDoc();
      // Bitset words live in targetPool so the scorer stays trivially destructible.
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);

      bool anyTerm = false;
      // Reclaim each term's DocsEnum allocations before scanning the next term.
      auto savepoint = targetPool.getSavePoint();
      while (fenum->next()) {
        anyTerm = true;
        {
          DocsEnum docsEnum(targetPool, segment.postingsReader(), fenum->terms());
          for (int32_t doc = docsEnum.nextDoc(); doc != PostingsReader::END; doc = docsEnum.nextDoc()) {
            bits.set(doc);
          }
        }
        targetPool.rewind(savepoint);
      }

      if (!anyTerm) return nullptr;  // the field exists but no term matched
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }
  };
};

} // namespace solux
