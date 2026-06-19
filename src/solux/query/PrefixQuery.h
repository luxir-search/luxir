#pragma once

#include <cstring>
#include <string_view>

#include "Query.h"
#include "solux/util/screaming.h"

namespace solux {

// Matches documents with a term in `field` starting with `prefix`.
// The prefix is used verbatim; an empty prefix matches documents that have the
// field. Every hit scores `boost`.
class PrefixQuery final : public solux::Query {
protected:
  std::string_view field;
  std::string_view prefix;
  float boost;
public:
  PrefixQuery(std::string_view field, std::string_view prefix, float boost = 1.0f)
    : field(field), prefix(prefix), boost(boost) {}

  std::string_view getField() const { return field; }
  std::string_view getPrefix() const { return prefix; }
  float getBoost() const { return boost; }

  PrefixQuery::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<PrefixQuery::Weight>(context, *this, flags);
  }

  class Weight final : public Query::Weight {
  protected:
    PrefixQuery& query;
    solux::CachedFieldInfo* cachedFieldInfo = nullptr;
  public:
    Weight(Context& context, PrefixQuery& query, int32_t flags)
      : Query::Weight(context, flags), query(query) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

    PrefixQuery::Scorer* createScorer(solux::MemPool& targetPool,
                                      solux::IndexReader::Segment& segment) override {
      if (cachedFieldInfo == nullptr) {
        return nullptr;  // field is not present in any segment
      }
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) {
        return nullptr;  // field is not present in this segment
      }

      // Use a fresh per-segment cursor because scorers can be built in parallel.
      auto* termsEnum = targetPool.make<TermsEnum>(targetPool, segment.postingsReader(), *segFieldInfo);
      std::string_view prefix = query.getPrefix();
      if (!termsEnum->seekCeil(prefix)) {
        return nullptr;  // no term is >= prefix, so none can start with it
      }

      int32_t maxDoc = segment.postingsReader().maxDoc();
      // Bitset words live in targetPool so the scorer stays trivially destructible.
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);

      bool anyTerm = false;
      // Reclaim each term's DocsEnum allocations before scanning the next term.
      auto savepoint = targetPool.getSavePoint();
      do {
        std::string_view term = (std::string_view)termsEnum->term();
        if (!term.starts_with(prefix)) break;  // terms are sorted: first miss ends the run
        anyTerm = true;
        {
          DocsEnum docsEnum(targetPool, segment.postingsReader(), *termsEnum);
          for (int32_t doc = docsEnum.nextDoc(); doc != PostingsReader::END; doc = docsEnum.nextDoc()) {
            bits.set(doc);
          }
        }
        targetPool.rewind(savepoint);
      } while (termsEnum->nextTerm());

      if (!anyTerm) {
        return nullptr;  // the ceil term existed but did not start with the prefix
      }
      return targetPool.make<PrefixQuery::Scorer>(bits, maxDoc, query.getBoost());
    }
  };

  class Scorer final : public Query::Scorer {
    FixedBitSet bits;
    int32_t maxDoc;
    float boost;
    int32_t docid = -1;

    // Position on the first set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }
  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float boost) : bits(bits), maxDoc(maxDoc), boost(boost) {}

    int32_t next() override {
      if (docid == PostingsReader::END) return docid;  // idempotent at END
      return advanceTo(docid + 1);
    }

    int32_t advance(int32_t target) override {
      assert(docid < target);  // strict advance
      return advanceTo(target);
    }

    int32_t docId() override { return docid; }

    float score() override { return boost; }
  };
};

} // namespace solux
