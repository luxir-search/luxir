// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OwnershipRelation.h"
#include "DomainVariantPlan.h"
#include "luxir/query/QueryPrep.h"

namespace luxir {

class OwnershipRelation::Scorer final : public Query::Scorer {
  Query::Scorer& child;
  const OwnershipMapper& mapper;
  OwnershipMapper::Cursor cursor;
  QueryPrep::DocSetScorer* domain;
  int32_t ownerCount;
  RowReduction reduction;
  bool maximum;
  bool scored;
  bool bounded;
  bool probing;
  Query::MembershipProof* proof;
  int32_t childCount;
  OwnershipShape::Family family{-1, 0, 0};
  int32_t verified = -1;
  bool accepted = false;
  float multiplier;
  int32_t childDoc;
  int32_t current = -1;
  float value = 0;
  ScoreBounds bounds;
  std::span<uint64_t> childBits;
  std::span<uint64_t> ownerBits;
  std::span<int64_t> boundaries;
  std::span<uint64_t> candidates;
  std::span<int32_t> candidateOwners;
  static constexpr int32_t CANDIDATE_ROWS = 4 * DocsEnumMeta::L1_DOCS;

  int32_t approximateOwner(int32_t target, int32_t end) {
    end = std::min(end, ownerCount);
    while (target < end) {
      if (domain) target = domain->advanceWindow(target, end);
      if (target >= end) break;
      family = cursor.seek(target);
      int32_t row = proof ? proof->nextPossible(family.first) : family.first;
      if (row < family.end) return current = target;
      if (target + 1 == end) break;
      if (row >= childCount) return current = PostingsReader::END;
      target = cursor.owner(row).owner;
    }
    return current = end == ownerCount ? PostingsReader::END : end;
  }

  bool countFamily(int32_t first, int32_t end, int64_t count = 0) {
    if ((int64_t)end - first + count < reduction.min) return false;
    int64_t stop = reduction.decidingCount();
    while (count < stop && first < end) {
      int32_t doc = child.advanceWindow(first, end);
      if (doc >= end) break;
      count++;
      first = doc + 1;
    }
    return reduction.accepts(count);
  }

  bool verifyOwner() {
    if (verified != current) {
      verified = current;
      accepted = current < ownerCount && (bounded ? countFamily(family.first, family.end)
          : child.advanceWindow(family.first, family.end) < family.end);
      value = multiplier;
    }
    return accepted;
  }

  static void setRange(std::span<uint64_t> words, int32_t first, int32_t end) {
    while (first < end) {
      int32_t stop = std::min(end, (first | 63) + 1);
      words[(size_t)first >> 6] |= (~0ULL << (first & 63)) & (~0ULL >> (63 - ((stop - 1) & 63)));
      first = stop;
    }
  }

  static bool any(std::span<uint64_t> words, int32_t first, int32_t end) {
    while (first < end) {
      int32_t stop = std::min(end, (first | 63) + 1);
      if (words[(size_t)first >> 6] & ((~0ULL << (first & 63)) & (~0ULL >> (63 - ((stop - 1) & 63))))) return true;
      first = stop;
    }
    return false;
  }

  int32_t advanceChild(int32_t target) {
    return scored ? child.advance(target) : child.advanceWindow(target, PostingsReader::END);
  }

  void fillOwnerWindow(std::span<uint64_t> words, int32_t start, int32_t end) {
    if (bounded) {
      auto selected = ownerBits.first((size_t)(end - start + 63) / 64);
      std::fill(selected.begin(), selected.end(), ~0ULL);
      if ((end - start) & 63) selected.back() &= (1ULL << ((end - start) & 63)) - 1;
      intersectCandidateBits(selected, start);
      for (size_t word = 0; word < selected.size(); word++) words[word] |= selected[word];
      return;
    }
    auto run = boundaries.first((size_t)(end - start) + 1);
    mapper.readBoundaries(start, end, run);
    int32_t row = (int32_t)run.front();
    int32_t last = (int32_t)run.back();
    // ends[i] is one past the last child row of owner start + i.
    const int64_t* ends = run.data() + 1;
    uint64_t* out = words.data();
    auto* eligible = domain;
    size_t owner = 0;
    // Marks the owner of a matching child row and returns the end of its
    // family. Owners only move forward within the window.
    auto familyEnd = [&](int32_t match) {
      while (ends[owner] <= match) owner++;
      int32_t doc = start + (int32_t)owner;
      if (!eligible || eligible->advanceWindow(doc, doc + 1) == doc) out[owner >> 6] |= 1ULL << (owner & 63);
      return (int32_t)ends[owner++];
    };
    if (childBits.empty()) {
      while ((row = child.advanceWindow(row, last)) < last) row = familyEnd(row);
      return;
    }
    uint64_t* bits = childBits.data();
    while (row < last) {
      int32_t limit = row + std::min(DocsEnumMeta::L1_DOCS, last - row);
      size_t wordCount = (size_t)(limit - row + 63) / 64;
      std::fill(bits, bits + wordCount, 0);
      child.fillWindowBits({bits, wordCount}, row, limit);
      int32_t next = row;
      size_t word = 0;
      uint64_t matches = bits[0];
      for (;;) {
        while (!matches && ++word < wordCount) matches = bits[word];
        if (!matches) break;
        int32_t match = row + (int32_t)(word * 64) + (int32_t)std::countr_zero(matches);
        // One matching child proves the family. Skip its remaining bits,
        // including any later child windows it spans.
        next = familyEnd(match);
        if (next >= limit) break;
        size_t offset = (size_t)(next - row);
        if ((offset >> 6) == word) {
          matches &= ~0ULL << (offset & 63);
        } else {
          word = offset >> 6;
          matches = bits[word] & (~0ULL << (offset & 63));
        }
      }
      // Only an empty fill needs a seek across a gap. Dense fills retain
      // the child's bulk decoding without a point probe before each one.
      row = next == row ? child.advanceWindow(limit, last) : std::max(limit, next);
    }
  }

public:
  Scorer(MemPool& pool, Query::Scorer& child, const OwnershipMapper& mapper,
         DocSet* domain, int32_t ownerCount, RowReduction reduction, float multiplier,
         const Query::ScorerShape& childShape, bool probing, Query::MembershipProof* proof, int32_t childCount)
      : child(child), mapper(mapper), cursor(mapper, pool),
        domain(domain ? pool.make<QueryPrep::DocSetScorer>(domain, ownerCount) : nullptr),
        ownerCount(ownerCount), reduction(reduction), maximum(reduction.kind == RowReduction::MAXIMUM),
        scored(reduction.kind != RowReduction::FIXED && reduction.kind != RowReduction::CONSTANT),
        bounded(reduction.bounded()), probing(probing), proof(proof),
        childCount(childCount), multiplier(multiplier), childDoc(child.docId()),
        childBits(!scored && childShape.windowFillClause == Query::ClauseShape::DIRECT
            ? pool.make_span<uint64_t>(DocsEnumMeta::L1_DOCS / 64) : std::span<uint64_t>{}),
        ownerBits(!scored && bounded ? pool.make_span<uint64_t>(DocsEnumMeta::L1_DOCS / 64) : std::span<uint64_t>{}),
        boundaries(!scored ? pool.make_span<int64_t>((size_t)std::min(ownerCount, DocsEnumMeta::L1_DOCS) + 1)
            : std::span<int64_t>{}),
        candidates(!scored && (probing || bounded) ? pool.make_span<uint64_t>(CANDIDATE_ROWS / 64) : std::span<uint64_t>{}),
        candidateOwners(!scored && (probing || bounded) ? pool.make_span<int32_t>(DocsEnumMeta::L1_DOCS) : std::span<int32_t>{}) {
    if (reduction.kind == RowReduction::CONSTANT) {
      auto score = child.constantContribution();
      if (!score) {
        // The weight proved uniformity, but a compound may not quote its
        // rounded sum. Read one deciding match through the bounded protocol.
        childDoc = child.advanceWindow(0, PostingsReader::END);
        if (childDoc == PostingsReader::END) this->ownerCount = 0;
        else score = child.score();
      }
      this->multiplier = score.value_or(0.0f) * multiplier;
    }
    if (!scored) bounds = ScoreBounds::exact(this->multiplier);
    else if (multiplier == 0.0f) bounds = ScoreBounds::exact(0.0f);
    else {
      // Capture whole-child bounds before iteration. The child may already
      // be beyond an owner's family when a parent asks for its score bound.
      auto childBounds = child.getScoreBounds(PostingsReader::END);
      if (maximum) {
        bounds = {childBounds.lo * multiplier,
                  std::min(childBounds.hi, child.getMaxScore(PostingsReader::END)) * multiplier};
        return;
      }
      double lo = childBounds.lo;
      double hi = std::min(childBounds.hi, child.getMaxScore(PostingsReader::END));
      double magnitude = std::max(std::abs(lo), std::abs(hi));
      if (reduction.kind == RowReduction::SUM) {
        if (reduction.max == std::numeric_limits<int32_t>::max()) {
          bounds = ScoreBounds::unknown();
          return;
        }
        lo *= lo < 0 ? reduction.max : reduction.min;
        hi *= hi > 0 ? reduction.max : reduction.min;
      }
      if (reduction.kind == RowReduction::SUM || reduction.kind == RowReduction::AVERAGE) {
        int64_t count = reduction.kind == RowReduction::SUM ? reduction.max : childCount;
        double error = (double)count * std::numeric_limits<double>::epsilon();
        error = magnitude * error / (1 - error);
        if (reduction.kind == RowReduction::SUM) error *= count;
        lo -= error;
        hi += error;
      }
      // Bounds round outward; reduction itself rounds only its final value.
      auto lower = (float)(lo * multiplier);
      auto upper = (float)(hi * multiplier);
      bounds = {std::nextafter(lower, -std::numeric_limits<float>::infinity()),
                std::nextafter(upper, std::numeric_limits<float>::infinity())};
    }
  }

  int32_t docId() override { return current; }
  float score() override { return value; }

  std::optional<float> constantContribution() const override {
    return scored ? std::nullopt : std::optional<float>(multiplier);
  }
  float getMaxScore(int32_t upTo) override { unused(upTo); return bounds.hi; }
  float getMaxScoreForSetup(int32_t upTo) override { unused(upTo); return bounds.hi; }
  ScoreBounds getScoreBounds(int32_t upTo) override { unused(upTo); return bounds; }
  int32_t advanceShallowForSetup(int32_t target) override { unused(target); return PostingsReader::END; }

  void setMinCompetitiveScore(float floor) override {
    if (!scored && floor > multiplier) ownerCount = 0;
    if (maximum && !bounded && multiplier > 0.0f) {
      child.setMinCompetitiveScore(Query::unscaleScoreFloor(floor, multiplier));
    }
  }

  int32_t next() override {
    return current == PostingsReader::END ? current : advance(current + 1);
  }

  int32_t approximationDocId() override { return current; }
  int32_t approximationNext() override {
    if (current == PostingsReader::END) return current;
    return probing ? approximateOwner(current + 1, ownerCount) : next();
  }
  int32_t approximationAdvance(int32_t target) override {
    return probing ? approximateOwner(target, ownerCount) : advance(target);
  }
  bool matches() override { return !probing || verifyOwner(); }

  int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
    return probing ? approximateOwner(min, max) : advanceWindow(min, max);
  }

  int32_t advance(int32_t target) override {
    assert(target > current);
    if (probing) {
      while (approximateOwner(target, ownerCount) < ownerCount) {
        if (verifyOwner()) return current;
        target = current + 1;
      }
      return current;
    }
    if (domain) {
      target = domain->docId() < target ? domain->advance(target) : domain->docId();
    }
    if (target >= ownerCount) return current = PostingsReader::END;
    auto family = cursor.seek(target);
    int32_t doc = childDoc;
    if (doc < family.first) doc = advanceChild(family.first);
    while (doc != PostingsReader::END) {
      // A child inside the sought family needs no lookup.
      if (doc >= family.end) family = cursor.owner(doc);
      int32_t owner = family.owner;
      if (domain) {
        int32_t eligible = domain->docId() < owner ? domain->advance(owner) : domain->docId();
        if (eligible != owner) {
          if (eligible >= ownerCount) return current = PostingsReader::END;
          family = cursor.seek(eligible);
          doc = advanceChild(family.first);
          continue;
        }
      }
      value = scored ? child.score() : 1.0f;
      if (maximum && !bounded) {
        while ((doc = child.next()) < family.end) value = std::max(value, child.score());
        value *= multiplier;
      } else if (scored) {
        int64_t count = 1;
        double sum = value;
        while ((doc = child.next()) < family.end) {
          if (++count > reduction.max) break;
          float score = child.score();
          if (maximum) value = std::max(value, score);
          else if (reduction.kind == RowReduction::MINIMUM) value = std::min(value, score);
          else sum += score;
        }
        if (!reduction.accepts(count)) {
          if (doc < family.end) doc = advanceChild(family.end);
          continue;
        }
        if (reduction.kind == RowReduction::SUM || reduction.kind == RowReduction::AVERAGE) {
          if (reduction.kind == RowReduction::AVERAGE) sum /= (double)count;
          value = (float)(sum * (double)multiplier);
        } else value *= multiplier;
      } else {
        if (bounded && !countFamily(doc + 1, family.end, 1)) {
          doc = advanceChild(family.end);
          continue;
        }
        value = multiplier;
      }
      // A competitive next() can finish without moving the postings cursor.
      childDoc = doc;
      return current = owner;
    }
    return current = PostingsReader::END;
  }

  int32_t advanceWindow(int32_t min, int32_t max) override {
    if (min >= max) return max;
    if (scored) return Query::Scorer::advanceWindow(min, max);
    if (probing) {
      while (min < max && approximateOwner(min, max) < max) {
        if (verifyOwner()) return current;
        min = current + 1;
      }
      return current = std::max(current, max >= ownerCount ? PostingsReader::END : max);
    }
    if (min >= ownerCount) return current = PostingsReader::END;
    max = std::min(max, ownerCount);
    if (domain) min = domain->advanceWindow(min, max);
    if (min >= max) return current = min;
    if (max == min + 1) {
      auto candidate = cursor.seek(min);
      if (bounded ? countFamily(candidate.first, candidate.end)
          : child.advanceWindow(candidate.first, candidate.end) < candidate.end) {
        value = multiplier;
        return current = min;
      }
      return current = max == ownerCount ? PostingsReader::END : max;
    }
    int32_t end = cursor.firstChild(max);
    int32_t row = cursor.firstChild(min);
    while ((row = child.advanceWindow(row, end)) < end) {
      auto family = cursor.owner(row);
      if (!domain || domain->advanceWindow(family.owner, family.owner + 1) == family.owner) {
        if (!bounded || countFamily(row + 1, family.end, 1)) {
          value = multiplier;
          return current = family.owner;
        }
        row = family.end;
        continue;
      }
      int32_t owner = domain->advanceWindow(family.owner + 1, max);
      if (owner >= max) return current = owner;
      row = cursor.firstChild(owner);
    }
    return current = max == ownerCount ? PostingsReader::END : max;
  }

  void fillWindowBits(std::span<uint64_t> words, int32_t start, int32_t end) override {
    if (scored) return Query::Scorer::fillWindowBits(words, start, end);
    skipCount(SkipStats::countBulkFillCalls);
    if (start == end || ownerCount == 0) return;
    assert(start >= 0 && start <= end && end <= ownerCount);
    current = end == ownerCount ? PostingsReader::END : end;
    for (int32_t first = start; first < end;) {
      int32_t limit = first + std::min(DocsEnumMeta::L1_DOCS, end - first);
      fillOwnerWindow(words.subspan((size_t)(first - start) / 64), first, limit);
      first = limit;
    }
  }

  void intersectCandidateBits(std::span<uint64_t> words, int32_t start) override {
    if (scored || candidates.empty()) return Query::Scorer::intersectCandidateBits(words, start);
    // Boundary scratch is owner-window sized; child scratch is independent
    // of family size. Oversized families use the bounded single-owner probe.
    int32_t end = std::min(ownerCount, start + (int32_t)words.size() * 64);
    for (int32_t lo = start; lo < end;) {
      int32_t hi = lo + std::min(DocsEnumMeta::L1_DOCS, end - lo);
      size_t count = 0;
      for (size_t w = (size_t)(lo - start) / 64; w < (size_t)(hi - start + 63) / 64; w++) {
        uint64_t bits = words[w];
        while (bits) {
          int32_t bit = (int32_t)std::countr_zero(bits);
          int32_t owner = start + (int32_t)(w * 64) + bit;
          if (domain && domain->advanceWindow(owner, owner + 1) != owner) words[w] &= ~(1ULL << bit);
          else candidateOwners[count++] = owner;
          bits &= bits - 1;
        }
      }
      if (SkipStats::enabled) SkipStats::joinCandidatesVisited += (int64_t)count;
      if (count && OwnershipShape::preferPointRanges(candidateOwners[0], candidateOwners[count - 1] + 1, count)) {
        if (SkipStats::enabled) SkipStats::joinBoundaryPointFamilies += (int64_t)count;
        for (size_t i = 0; i < count; i++) {
          int32_t owner = candidateOwners[i];
          auto candidate = cursor.seek(owner);
          if ((proof && proof->nextPossible(candidate.first) >= candidate.end)
              || (bounded ? !countFamily(candidate.first, candidate.end)
                  : child.advanceWindow(candidate.first, candidate.end) >= candidate.end)) {
            int32_t offset = owner - start;
            words[(size_t)offset >> 6] &= ~(1ULL << (offset & 63));
          }
        }
      } else if (count) {
        skipCount(SkipStats::joinBoundaryRuns);
        int32_t firstOwner = candidateOwners[0];
        int32_t lastOwner = candidateOwners[count - 1] + 1;
        auto run = boundaries.first((size_t)(lastOwner - firstOwner) + 1);
        mapper.readBoundaries(firstOwner, lastOwner, run);
        auto firstRow = [&](size_t candidate) { return (int32_t)run[(size_t)(candidateOwners[candidate] - firstOwner)]; };
        auto endRow = [&](size_t candidate) { return (int32_t)run[(size_t)(candidateOwners[candidate] - firstOwner) + 1]; };
        auto reject = [&](size_t candidate) {
          int32_t offset = candidateOwners[candidate] - start;
          words[(size_t)offset >> 6] &= ~(1ULL << (offset & 63));
        };
        for (size_t first = 0; first < count;) {
          int32_t base = firstRow(first);
          if (endRow(first) - base > CANDIDATE_ROWS) {
            int32_t limit = endRow(first);
            if ((proof && proof->nextPossible(base) >= limit)
                || (bounded ? !countFamily(base, limit) : child.advanceWindow(base, limit) >= limit)) reject(first);
            first++;
            continue;
          }
          size_t stop = first + 1;
          int32_t limit;
          if (bounded) {
            // Complete small families together. Only the tail can be partial;
            // finish it before advancing the child into a later family.
            int64_t decide = reduction.decidingCount();
            while (stop < count && endRow(stop - 1) - firstRow(stop - 1) <= decide
                && endRow(stop) - base <= CANDIDATE_ROWS) stop++;
            limit = (int32_t)std::min<int64_t>(endRow(stop - 1), (int64_t)firstRow(stop - 1) + decide);
          } else {
            while (stop < count && endRow(stop) - base <= CANDIDATE_ROWS) stop++;
            limit = endRow(stop - 1);
          }
          auto bits = candidates.first((size_t)(limit - base + 63) / 64);
          std::fill(bits.begin(), bits.end(), 0);
          for (size_t i = first; i < stop; i++) setRange(bits, firstRow(i) - base, std::min(limit, endRow(i)) - base);
          if (proof) proof->intersect(bits, base);
          child.intersectCandidateBits(bits, base);
          for (size_t i = first; i < stop; i++) {
            if (!bounded) {
              if (!any(bits, firstRow(i) - base, endRow(i) - base)) reject(i);
            } else {
              int64_t matches = 0;
              int32_t row = firstRow(i) - base;
              int32_t last = std::min(limit, endRow(i)) - base;
              while (row < last) {
                int32_t next = std::min(last, (row | 63) + 1);
                matches += std::popcount(bits[(size_t)row >> 6]
                    & ((~0ULL << (row & 63)) & (~0ULL >> (63 - ((next - 1) & 63)))));
                row = next;
              }
              if (limit < endRow(i) && matches < reduction.decidingCount()) {
                if (!countFamily(limit, endRow(i), matches)) reject(i);
              } else {
                if (!reduction.accepts(matches)) reject(i);
              }
            }
          }
          first = stop;
        }
      }
      lo = hi;
    }
    current = end == ownerCount ? PostingsReader::END : end;
  }

  [[gnu::cold]] void prepareMembership() override {
    if (!scored) child.prepareMembership();
  }
};

namespace {
class OwnerProof final : public Query::MembershipProof {
  Query::MembershipProof& child;
  OwnershipMapper::Cursor cursor;
  int32_t owners;
  int32_t children;

  Range nextRange(int32_t target) override {
    if (target >= owners) return {PostingsReader::END, PostingsReader::END};
    auto family = cursor.seek(target);
    int32_t row = child.nextPossible(family.first);
    if (row >= children) return {PostingsReader::END, PostingsReader::END};
    int32_t first = row < family.end ? target : cursor.owner(row).owner;
    int32_t last = cursor.owner(std::min(children, child.possibleEnd()) - 1).owner + 1;
    return {first, last};
  }

public:
  OwnerProof(MemPool& pool, Query::MembershipProof& child, const OwnershipMapper& mapper,
             int32_t owners, int32_t children)
      : child(child), cursor(mapper, pool), owners(owners), children(children) {}
};
} // namespace

class OwnershipRelation::Supplier final : public Query::ScorerSupplier {
  Query::ScorerSupplier& child;
  const OwnershipMapper& mapper;
  DocSet* domain;
  int32_t ownerCount;
  int32_t childCount;
  RowReduction reduction;
  float multiplier;

  // hugin, 5M structured roots, 2026-11-16: mapping a filled owner costs
  // 4.4 ns; pulling costs 45-90 ns per body candidate. Use a nominal four
  // instructions/ns to compare with MembershipWork, charging ranked probes
  // at the upper end and count probes at the lower end. Child production
  // is additional, so expensive children move the crossover toward pull.
  static constexpr double OWNER_FILL_INSTRUCTIONS = 18;
  static constexpr double RANKED_PROBE_INSTRUCTIONS = 360;
  static constexpr double COUNT_PROBE_INSTRUCTIONS = 180;

  class Plan final : public Query::ScorerPlan {
    Supplier& supplier;
    Query::ScorerPlan& child;
    bool probing;

    Query::Scorer* buildScorer(MemPool& pool) override {
      auto* scorer = child.build(pool);
      return scorer ? pool.make<Scorer>(pool, *scorer, supplier.mapper, supplier.domain,
          supplier.ownerCount, supplier.reduction, supplier.multiplier, child.shape(), probing,
          probing ? child.buildMembershipProof(pool) : nullptr, supplier.childCount) : nullptr;
    }

  public:
    Plan(Supplier& supplier, const Query::PlanContext& context, const Query::ScorerShape& shape,
         Query::ScorerPlan& child, bool probing)
        : Query::ScorerPlan(context, shape, supplier.cost()),
          supplier(supplier), child(child), probing(probing) {}

    Query::MembershipProof* buildMembershipProof(MemPool& pool) override {
      auto* proof = child.buildMembershipProof(pool);
      return proof ? pool.make<OwnerProof>(pool, *proof, supplier.mapper, supplier.ownerCount, supplier.childCount) : nullptr;
    }
  };

  Query::PlanContext childContext(Query::PlanContext context, Query::MembershipConsumption consumption) const {
    if (reduction.kind != RowReduction::FIXED && reduction.kind != RowReduction::CONSTANT) {
      context.membershipConsumption = Query::MembershipConsumption::WINDOWS;
      return context;
    }
    auto scale = [&](int64_t count) {
      if (!ownerCount) return (int64_t)0;
      return std::min<int64_t>(childCount, (count >= ownerCount ? (int64_t)childCount
          : (count * childCount + ownerCount - 1) / ownerCount));
    };
    context.demand.candidates = scale(context.demand.candidates);
    context.demand.span = scale(context.demand.span);
    context.expectedSpan = scale(context.expectedSpan);
    if (context.demand.expectedCandidates >= 0 && ownerCount) {
      context.demand.expectedCandidates *= (double)childCount / ownerCount;
    }
    context.membershipConsumption = consumption;
    // Some opaque suppliers only expose window consumption. Their bounded
    // protocol still verifies a family without exposing a candidate arm.
    if (consumption == Query::MembershipConsumption::CANDIDATES
        && child.describeScorer(context).membershipConsumption != consumption) {
      context.membershipConsumption = Query::MembershipConsumption::WINDOWS;
    }
    return context;
  }

  bool fillsProfitably(const Query::PlanContext& context) const {
    if (!ownerCount) return false;
    const auto& demand = context.demand;
    double candidates = demand.expectedCandidates;
    if (candidates < 0) {
      if (demand.candidates == std::numeric_limits<int64_t>::max()) return false;
      candidates = (double)demand.candidates;
    }
    if (!std::isfinite(candidates)) return false;
    candidates = std::clamp(candidates, 0.0, (double)std::min<int64_t>(demand.candidates, ownerCount));
    // The context has a span, not its positions. Estimate the mapped child
    // exposure at the same fraction; never inspect ownership during planning.
    // A fill covers whole windows, and each opportunity can expose one,
    // whatever span the consumer quoted for its own cursor.
    int64_t exposed = std::min<int64_t>(demand.candidates, ownerCount) * DocsEnumMeta::L1_DOCS;
    double owners = (double)std::clamp<int64_t>(
        std::min(std::max(demand.span, exposed), context.expectedSpan), 0, ownerCount);
    double production = child.membershipWork(childCount).produce;
    double fill = OWNER_FILL_INSTRUCTIONS * owners + production * owners / ownerCount;
    double probe = candidates * (demand.horizon == Query::ExecutionUse::COUNT_WINDOWS
        ? COUNT_PROBE_INSTRUCTIONS : RANKED_PROBE_INSTRUCTIONS);
    return fill < probe;
  }

  bool probe(const Query::PlanContext& context) const {
    using Consumption = Query::MembershipConsumption;
    if (reduction.kind != RowReduction::FIXED && reduction.kind != RowReduction::CONSTANT) return false;
    if (context.membershipConsumption == Consumption::CANDIDATES) return true;
    if (context.membershipConsumption == Consumption::EITHER) {
      auto shape = child.describeScorer(childContext(context, Consumption::EITHER));
      if (shape.membershipConsumption == Consumption::CANDIDATES) return true;
      // A priced child chose production over verification. Opaque children
      // without candidate quotes retain the cardinality fallback.
      if (child.quoteMembership(childContext(context, Consumption::CANDIDATES))) return false;
      return context.demand.candidates < std::min<int64_t>(ownerCount, child.cost());
    }
    return context.demand.candidates < std::min<int64_t>(ownerCount, child.cost())
        && (context.demand.horizon == Query::ExecutionUse::PULL || !fillsProfitably(context));
  }

  struct Selection {
    Query::PlanContext childContext;
    Query::ScorerShape shape;
    bool probing;
  };

  // Candidate consumers choose verification through the child's scaled
  // demand. Explicit window consumers still need the fill/probe comparison:
  // their filter gates price cardinality rather than membership production.
  // The latter estimate uses the body's cost, not its post-pruning survivors,
  // and window exposure rather than known positions. A consumer-side price
  // comparison would remove both guesses.
  Selection selectScorer(const Query::PlanContext& context) const {
    bool probing = probe(context);
    Selection selection{childContext(context, probing ? Query::MembershipConsumption::CANDIDATES
        : Query::MembershipConsumption::WINDOWS), {}, probing};
    auto& shape = selection.shape;
    shape.matchState = child.describeScorer(selection.childContext).matchState;
    if (reduction.bounded() && shape.matchState == Query::MatchState::NONEMPTY) {
      shape.matchState = Query::MatchState::UNKNOWN;
    }
    shape.directKind = Query::DirectScorerKind::OTHER;
    shape.reportedTwoPhase = probing ? Query::ReportedTwoPhase::YES : Query::ReportedTwoPhase::NO;
    shape.windowFillClause = Query::ClauseShape::NONE;
    shape.termDisjunctionClause = shape.termConjunctionClause = Query::ClauseShape::NONE;
    shape.independentTerm = Query::IndependentTermAccess::UNSUPPORTED;
    shape.docsOnly = Query::DocsOnlyAccess::UNSUPPORTED;
    shape.directDocSet = Query::DirectDocSetAccess::UNSUPPORTED;
    shape.membershipConsumption = context.membershipConsumption == Query::MembershipConsumption::EITHER
        ? (probing ? Query::MembershipConsumption::CANDIDATES : Query::MembershipConsumption::WINDOWS)
        : context.membershipConsumption;
    if (probing && shape.membershipConsumption == Query::MembershipConsumption::CANDIDATES) {
      shape.windowFillClause = Query::ClauseShape::DIRECT;
      return selection;
    }
    if ((reduction.kind != RowReduction::FIXED && reduction.kind != RowReduction::CONSTANT)
        || ownerCount == 0 || probing) return selection;
    if (context.membershipConsumption == Query::MembershipConsumption::EITHER) {
      shape.windowFillClause = Query::ClauseShape::DIRECT;
      return selection;
    }
    if (fillsProfitably(context)) shape.windowFillClause = Query::ClauseShape::DIRECT;
    return selection;
  }

public:
  Supplier(Query::ScorerSupplier& child, const OwnershipMapper& mapper,
           DocSet* domain, int32_t ownerCount, int32_t childCount, RowReduction reduction, float multiplier)
      : child(child), mapper(mapper), domain(domain), ownerCount(ownerCount), childCount(childCount),
        reduction(reduction), multiplier(multiplier) {}

  int64_t cost() override { return std::min(child.cost(), (int64_t)ownerCount); }

  Query::ScorerShape describeScorer(const Query::PlanContext& context) const override {
    return selectScorer(context).shape;
  }
  Query::UnresolvedSupplierCause unresolvedScorerCause(const Query::PlanContext& context) const override {
    return child.unresolvedScorerCause(selectScorer(context).childContext);
  }
  bool fillExpansionMemo(const Query::PlanContext& context) override {
    return child.fillExpansionMemo(selectScorer(context).childContext);
  }
  Query::ScorerPlan* resolve(MemPool& pool, const Query::PlanContext& context) override {
    auto selected = selectScorer(context);
    return pool.make<Plan>(*this, context, selected.shape,
        *child.resolve(pool, selected.childContext), selected.probing);
  }
};

OwnershipRelation::OwnershipRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory)
    : RowRelation(many, one, memory) {
  auto path = many.path();
  if (path == one.path()) throw RequestError("domain transform requires different spaces");
  while (!path.empty() && path != one.path()) path = spaceParentPath(path);
  if (path != one.path()) throw RequestError("domain transform requires ancestor and descendant paths");
  mappers.reserve(many.segments().size());
  for (size_t leaf = 0; leaf < many.segments().size(); leaf++) {
    auto& child = many.segments()[leaf];
    assert(&child.segmentCore() == &one.segments()[leaf].segmentCore());
    if (child.hasSpace()) mappers.emplace_back(std::in_place, child.segmentCore(), one.path(), many.path());
    else mappers.emplace_back(std::nullopt);
  }
}

Query::ScorerSupplier* OwnershipRelation::reduce(MemPool& pool, size_t leaf,
    Query::ScorerSupplier& input, DocSet* domain, RowReduction reduction, float multiplier) {
  assert(reduction.kind != RowReduction::NONE);
  return mappers[leaf] ? pool.make<Supplier>(input, *mappers[leaf], domain,
      one.segments()[leaf].maxDoc(), many.segments()[leaf].maxDoc(), reduction, multiplier) : nullptr;
}

FilterKeyScope OwnershipRelation::appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext&) const {
  out.appendByte(0); // ownership relation
  out.appendString(many.path());
  out.appendString(one.path());
  return FilterKeyScope::SEGMENT_STABLE;
}

DomainHandle OwnershipRelation::expand(size_t leaf, DomainHandle domain) {
  auto& source = one.segments()[leaf];
  auto& destination = many.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  if (mapper) {
    if (!domain.get()) return DomainHandle().scoped(destination);
    if (domain.get() == source.liveDocSet()) {
      return DomainHandle::pinned(destination.liveDocSet(), destination.liveDocsShared()).scoped(destination);
    }
  }
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && domain.get()) {
    domain.get()->forEach([&](int32_t row) {
      auto [first, end] = mapper->range(row);
      builder.addRange(first, end);
    });
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

DomainHandle OwnershipRelation::project(size_t leaf, DomainHandle domain) {
  auto& source = many.segments()[leaf];
  auto& destination = one.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && source.maxDoc()) {
    MemPool pool;
    OwnershipMapper::Cursor cursor(*mapper, pool);
    if (domain.get()) {
      int32_t lastOwner = -1;
      domain.get()->forEach([&](int32_t row) {
        int32_t owner = cursor.owner(row).owner;
        if (owner != lastOwner) builder.add(owner);
        lastOwner = owner;
      });
    } else {
      // All children still project to owners with nonempty families only.
      for (int32_t child = 0; child < source.maxDoc();) {
        auto family = cursor.owner(child);
        builder.add(family.owner);
        child = family.end;
      }
    }
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

} // namespace luxir
