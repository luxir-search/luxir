#pragma once

#include <faiss/Index.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Query.h"
#include "protos/solux_types.pb.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/reader/VectorReader.h"
#include "solux/util/log.h"
#include "solux/util/screaming.h"

namespace solux {

/// kNN query against a vector field's FAISS aux index ("vec.<field>").
///
/// Execution model: the FAISS index is shard-level (one per IndexReader, not
/// per segment), so search runs once at Weight construction.  Hits are mapped
/// back to (segment, docId) via the per-segment cumulative vector counts and
/// bucketed per segment in docId order for the segment-parallel TopDocsReq
/// pipeline.
///
/// LiveDocs filtering happens *inside* the FAISS search via a custom
/// faiss::IDSelector, so deleted-doc vectors are skipped server-side and we
/// always get exactly k live results back (or fewer if the index has fewer
/// than k live vectors).  No over-fetch heuristic needed.
///
/// Multi-valued vector fields are supported: every vector is its own FAISS id,
/// mapped back to its owning doc via the segment's valueRank->docId column
/// (VectorReader::docForVectorRank).  Several of a doc's vectors can land in the
/// result; they are collapsed to one hit per doc keeping the best ("max-sim")
/// score.  NOTE: we still request k *vectors* from FAISS, so a multi-valued query
/// can return fewer than k *docs* when a doc owns several of the top vectors.
/// Iterative over-fetch to guarantee k docs is a future enhancement.
class KnnQuery final : public solux::Query {
  std::string_view field;
  std::span<const float> queryVec;
  int32_t k;

public:
  /// One result hit within a segment.  docId is the local docRank within the
  /// segment; score is converted to "higher is better" regardless of FAISS
  /// metric (see scoreFromDist).
  struct Hit {
    int32_t docId;
    float score;
  };

  /// Resolves a segment-local value rank to its owning docId.  Exactly one mode is
  /// active per segment:
  ///   - mono   : multi-valued reverse map (valueRank -> docId), read off the column.
  ///   - sel    : sparse single-valued (some docs lack the field) -> docId is the
  ///              valueRank-th set bit of the has-field bitset (select()).  No
  ///              per-vector array; the Selector caches only a small per-bucket
  ///              prefix and reuses the column's existing bitset.
  ///   - neither: dense single-valued, valueRank == docId (identity).
  /// The MonoReader / Selector must outlive every resolve() call (both live for the
  /// duration of the FAISS search + result walk inside Weight's constructor).
  class SegV2D {
  public:
    MonoReader* mono = nullptr;
    const screaming::BitSet::Selector* sel = nullptr;
    int32_t resolve(int64_t valueRank) const {
      if (mono) return (int32_t)mono->valueAt(valueRank);
      if (sel) return sel->select((int32_t)valueRank);
      return (int32_t)valueRank;
    }
  };

  KnnQuery(std::string_view field, std::span<const float> queryVec, int32_t k)
    : field(field), queryVec(queryVec), k(k) {}

  std::string_view getField() const noexcept { return field; }
  std::span<const float> getQueryVec() const noexcept { return queryVec; }
  int32_t getK() const noexcept { return k; }

  Query::Weight* createWeight(Query::Context& context) override {
    return context.pool.make<KnnQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    KnnQuery& query;
    std::span<std::span<const Hit>> perSegHits;

  public:
    Weight(Query::Context& context, KnnQuery& query)
      : Query::Weight(context), query(query) {
      IndexReader& reader = context.topReader;
      size_t numSegs = reader.segments().size();

      // Default: empty per-seg buckets; createScorer returns nullptr for
      // every segment.  Reset only when we successfully run a search.
      perSegHits = context.pool.make_span<std::span<const Hit>>(numSegs);
      for (auto& s : perSegHits) s = {};

      // Find the aux reader for this field.  Missing (e.g. field never had
      // its FAISS index built) yields zero hits - graceful degradation.
      std::string auxName("vec.");
      auxName.append(query.getField());
      auto aux = reader.getAuxReader(auxName);
      if (!aux) {
        LOG_DEBUG("KnnQuery: no aux index '{}' on this reader; returning empty result", auxName);
        return;
      }
      auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
      if (!vaux) {
        throw std::runtime_error(std::format(
          "KnnQuery: aux '{}' is not a vector index (kind={})", auxName, aux->getKind()));
      }

      if ((int32_t)query.getQueryVec().size() != vaux->getDims()) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector dims {} do not match index dims {} for field '{}'",
          query.getQueryVec().size(), vaux->getDims(), query.getField()));
      }

      faiss::Index* faissIdx = vaux->getFaissIndex();
      if (!faissIdx || faissIdx->ntotal == 0) return;

      // Per-segment vector counts -> cumulative prefix for FAISS-id -> segment
      // mapping.  Builder added segments in ord order skipping zero-vector
      // ones, and our IndexInfo segments are in the same canonical order, so
      // the prefix sum lines up: zero-vector segments contribute a zero range.
      //
      // Within a segment, FAISS ids correspond to *value ranks* (0 ..
      // numVectors).  Each segment gets a SegV2D resolver mapping valueRank ->
      // docId: identity for dense single-valued, a materialized bitset lookup for
      // sparse single-valued, and the column's reverse map for multi-valued.
      std::span<int64_t> prefix = context.pool.make_span<int64_t>(numSegs + 1);
      prefix[0] = 0;
      std::span<SegV2D> v2dPerSeg = context.pool.make_span<SegV2D>(numSegs);
      for (auto& s : v2dPerSeg) s = SegV2D{};

      // Holds the multi-valued segments' valueRank->docId MonoReaders alive through
      // the FAISS search + result walk below (both in this constructor).  MonoReader is
      // just pointers into the segment mmap (valid for the whole query), so copying it
      // out of the scratch VectorReader is safe and cheap.  (The sparse single-valued
      // Selector is pool-allocated via makeValueDocSelector, so it needs no holder.)
      std::vector<std::optional<MonoReader>> monoHolders(numSegs);

      auto segInfos = context.readSegInfos(query.getField());
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = 0;
        if (!segInfos.empty() && segInfos[i] != nullptr) {
          // FIXED_SIZE vector column.  numVectors == #docs-with-field for
          // single-valued, or the total vector count for multi-valued.
          VectorReader vr(reader.segments()[i].postingsReader(), *segInfos[i]);
          segCount = vr.numVectors();
          if (segCount > 0) {
            if (vr.isMultiValued()) {
              // valueRank -> docId comes straight off the column's reverse map.
              MonoReader* mr = vr.strColReader().getValDocReader();
              if (mr == nullptr) {
                throw std::runtime_error(std::format(
                  "KnnQuery: multi-valued vector field '{}' is missing its valueRank->docId "
                  "map (segment predates the map); reindex required", query.getField()));
              }
              monoHolders[i].emplace(*mr);
              v2dPerSeg[i].mono = &*monoHolders[i];
            } else {
              // Single-valued: dense => identity (valueRank == docId).  Sparse (some
              // docs lack the field) => resolve via select() on the has-field bitset:
              // docId is the valueRank-th set bit.  No per-vector array; the Selector
              // caches only a small per-bucket prefix (nBuckets+1 ints) and reuses the
              // column's bitset.
              auto& dr = vr.strColReader().docsReader();
              if (dr.hasBitset()) {
                v2dPerSeg[i].sel = makeValueDocSelector(context.pool, dr.bitset());
              }
            }
          }
        }
        prefix[i + 1] = prefix[i] + segCount;
      }
      if (prefix[numSegs] != faissIdx->ntotal) {
        // Drift between the FAISS index and the column data - implies the aux
        // entry was built against a different segment composition than what's
        // currently published.  Should be impossible because IndexWriter
        // invalidates aux entries on coreGen change.
        throw std::runtime_error(std::format(
          "KnnQuery: FAISS ntotal={} != sum of per-segment vector counts={} for field '{}'",
          faissIdx->ntotal, prefix[numSegs], query.getField()));
      }

      // Optionally normalize the query for COSINE - stored vectors were
      // normalized at build, so we need a unit query for cosine = IP semantics
      // to hold.  Done into a local copy; the caller's span is unmodified.
      std::vector<float> queryBuf;
      const float* queryPtr = query.getQueryVec().data();
      if (vaux->getMetric() == proto::VectorParams::COSINE) {
        queryBuf.assign(query.getQueryVec().begin(), query.getQueryVec().end());
        faiss::fvec_renorm_L2((size_t)vaux->getDims(), 1, queryBuf.data());
        queryPtr = queryBuf.data();
      }

      // Use a faiss::IDSelector to filter deleted docs *inside* the search,
      // so we ask for exactly k and never under-deliver due to liveDocs
      // post-filtering.  The selector closes over `prefix`, `v2dPerSeg`, and
      // segment liveDocs - all live for the duration of search() (pool /
      // IndexReader scoped).
      LiveDocsSelector selector(prefix, v2dPerSeg, reader.segments());
      faiss::SearchParameters params;
      params.sel = &selector;

      int64_t kReq = std::min((int64_t)query.getK(), faissIdx->ntotal);
      if (kReq <= 0) return;

      std::vector<faiss::idx_t> ids((size_t)kReq);
      std::vector<float> dists((size_t)kReq);
      faissIdx->search(1, queryPtr, kReq, dists.data(), ids.data(), &params);

      // Walk FAISS results.  IDs are -1 when fewer than kReq live vectors
      // exist; bucket the rest per segment (sorted by docId for the
      // PostingsReader contract).
      std::vector<std::vector<Hit>> perSegBuf(numSegs);
      for (int64_t i = 0; i < kReq; i++) {
        faiss::idx_t fid = ids[i];
        if (fid < 0) continue;  // FAISS pad - no more live results
        int32_t ord = findSeg((int64_t)fid, prefix);
        if (ord < 0) continue;
        int64_t valueRank = (int64_t)fid - prefix[ord];
        int32_t docId = v2dPerSeg[ord].resolve(valueRank);
        perSegBuf[ord].push_back({docId, scoreFromDist(dists[i], vaux->getMetric())});
      }

      // Materialize the per-segment hit arrays in the pool, sorted by docId.
      for (size_t ord = 0; ord < numSegs; ord++) {
        auto& bucket = perSegBuf[ord];
        if (bucket.empty()) continue;
        std::sort(bucket.begin(), bucket.end(),
                  [](const Hit& a, const Hit& b) { return a.docId < b.docId; });
        // Collapse multiple hits from the same doc (multi-valued: several of a doc's
        // vectors made the cut) into one, keeping the best score.  scoreFromDist is
        // higher-is-better for every metric, so max == closest.  No-op when docIds
        // are already unique (single-valued).
        size_t w = 0;
        for (size_t r = 0; r < bucket.size();) {
          int32_t d = bucket[r].docId;
          float best = bucket[r].score;
          size_t j = r + 1;
          for (; j < bucket.size() && bucket[j].docId == d; j++)
            best = std::max(best, bucket[j].score);
          bucket[w++] = {d, best};
          r = j;
        }
        bucket.resize(w);
        auto out = context.pool.make_span<Hit>(bucket.size());
        std::copy(bucket.begin(), bucket.end(), out.begin());
        perSegHits[ord] = out;
      }
    }

    Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) override {
      if ((size_t)segment.ord >= perSegHits.size()) return nullptr;
      auto hits = perSegHits[segment.ord];
      if (hits.empty()) return nullptr;
      return target.make<KnnQuery::Scorer>(hits);
    }

    // Exposed for testing.
    std::span<std::span<const Hit>> getPerSegHits() const noexcept { return perSegHits; }
  };

  class Scorer final : public Query::Scorer {
    std::span<const Hit> hits;
    int32_t cur = -1;

  public:
    explicit Scorer(std::span<const Hit> hits) noexcept : hits(hits) {}

    int32_t next() override {
      cur++;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    int32_t docId() override {
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    float score() override {
      return hits[cur].score;
    }
  };

private:
  // Convert FAISS's per-metric distance to a "higher is better" score.
  //   L2:    FAISS returns squared distance - invert to 1/(1+d).
  //   IP:    FAISS returns dot product - already higher-is-better.
  //   COSINE: stored as IP over normalized vectors (builder normalized,
  //           query is normalized at search time) - same as IP.
  static float scoreFromDist(float dist, int32_t metric) {
    switch (metric) {
      case proto::VectorParams::L2:
        return 1.0f / (1.0f + dist);
      case proto::VectorParams::IP:
      case proto::VectorParams::COSINE:
        return dist;
      default:
        // Unknown metric (older builds without opaque_meta).  Return raw
        // distance; caller can sort but absolute meaning is unspecified.
        return dist;
    }
  }

  // Locate the segment ord that "owns" the given FAISS id, given a
  // prefix-sum array of length numSegs+1 (prefix[0]=0, prefix[i] = sum of
  // vector counts in segments [0..i)).  Returns -1 if out of range.
  static int32_t findSeg(int64_t faissId, std::span<const int64_t> prefix) {
    if (faissId < 0 || faissId >= prefix.back()) return -1;
    auto it = std::upper_bound(prefix.begin(), prefix.end(), faissId);
    return (int32_t)(it - prefix.begin() - 1);
  }

  /// faiss::IDSelector that maps a FAISS id to (segment ord, docRank) via
  /// the precomputed prefix sums + per-segment valueRank->docRank lookup,
  /// then consults the segment's liveDocs.  FAISS calls is_member(id) for
  /// every candidate during search and skips those that return false - so
  /// deleted-doc vectors never make it into the result list.
  ///
  /// For IndexFlat this doesn't reduce the scan cost (we still touch every
  /// vector for distance computation) but it cleanly avoids over-fetching.
  /// For HNSW/IVF later, the selector can also prune graph traversal / list
  /// selection.
  class LiveDocsSelector : public faiss::IDSelector {
    std::span<const int64_t> prefix;
    // Per-segment valueRank -> docId resolver (mono / flat / identity).
    std::span<const SegV2D> v2dPerSeg;
    std::span<IndexReader::Segment> segs;

  public:
    LiveDocsSelector(std::span<const int64_t> prefix,
                     std::span<const SegV2D> v2dPerSeg,
                     std::span<IndexReader::Segment> segs) noexcept
      : prefix(prefix), v2dPerSeg(v2dPerSeg), segs(segs) {}

    bool is_member(faiss::idx_t id) const final {
      int32_t ord = findSeg((int64_t)id, prefix);
      if (ord < 0) return false;
      auto* live = segs[ord].liveDocs();
      if (live == nullptr) return true;  // no deletes in this segment
      int64_t valueRank = (int64_t)id - prefix[ord];
      int32_t docId = v2dPerSeg[ord].resolve(valueRank);
      return live->bitset().get(docId);
    }
  };
};

} // namespace solux
