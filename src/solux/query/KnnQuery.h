#pragma once

#include <faiss/Index.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

#include "Query.h"
#include "protos/solux_types.pb.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/reader/VectorReader.h"
#include "solux/query/VectorEngine.h"
#include "solux/search/DocSet.h"
#include "solux/util/BranchlessSearch.h"
#include "solux/util/log.h"
#include "solux/util/screaming.h"

namespace solux {

/// kNN query against a vector field's FAISS aux index ("vec.<field>").
///
/// Execution model: the FAISS index is currently shard-level (one per
/// IndexReader, not per segment), so search materializes once during the query
/// preparation phase.  Hits cross a small vector engine seam as
/// (segment, valueRank, score), then are collapsed to docs and bucketed per
/// segment in docId order for the segment-parallel TopDocsReq pipeline.
///
/// The active query domain (liveDocs intersected with any enclosing filter
/// clauses) is applied *inside* the FAISS search via a custom faiss::IDSelector
/// (DomainSelector), so deleted-doc and out-of-domain vectors are skipped during
/// the search and we over-request vectors only to fill k distinct docs after
/// multi-vector collapse.
///
/// Multi-valued vector fields are supported: every vector is its own FAISS id,
/// mapped back to its owning doc via the segment's valueRank->docId column
/// (VectorReader::docForVectorRank).  Several of a doc's vectors can land in the
/// result; they are collapsed to one hit per doc keeping the best ("max-sim")
/// score.  The query adaptively over-requests vectors, bounded by
/// maxKnnCandidates, so it normally emits k distinct docs (or fewer if fewer
/// live docs have a vector).  Above that cap the result is best-effort.
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

  // Maximum vectors requested from FAISS while trying to fill k distinct docs.
  // Mutable so tests can shrink it to exercise the best-effort cap path.
  static inline int64_t maxKnnCandidates = 1000000;

  /// Resolves a segment-local value rank to its owning docId.  Exactly one mode is
  /// active per segment:
  ///   - mono   : multi-valued reverse map (valueRank -> docId), read off the column.
  ///   - sel    : sparse single-valued (some docs lack the field) -> docId is the
  ///              valueRank-th set bit of the has-field bitset (select()).  No
  ///              per-vector array; the Selector caches only a small per-bucket
  ///              prefix and reuses the column's existing bitset.
  ///   - neither: dense single-valued, valueRank == docId (identity).
  /// The MonoReader / Selector must outlive every resolve() call (both live for the
  /// duration of the FAISS search + result walk inside Weight::prepare).
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

  struct DocHit {
    int32_t segOrd;
    int32_t docId;
    float score;
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
    VectorAuxReader* vaux = nullptr;

  public:
    Weight(Query::Context& context, KnnQuery& query)
      : Query::Weight(context), query(query) {
      IndexReader& reader = context.topReader;

      // Find the aux reader for this field.  Missing (e.g. field never had
      // its FAISS index built) yields zero hits - graceful degradation.
      std::string auxName("vec.");
      auxName.append(query.getField());
      auto aux = reader.getAuxReader(auxName);
      if (!aux) {
        LOG_DEBUG("KnnQuery: no aux index '{}' on this reader; returning empty result", auxName);
        return;
      }
      vaux = dynamic_cast<VectorAuxReader*>(aux.get());
      if (!vaux) {
        throw std::runtime_error(std::format(
          "KnnQuery: aux '{}' is not a vector index (kind={})", auxName, aux->getKind()));
      }

      if ((int32_t)query.getQueryVec().size() != vaux->getDims()) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector dims {} do not match index dims {} for field '{}'",
          query.getQueryVec().size(), vaux->getDims(), query.getField()));
      }

    }

    bool needsPrepare() const noexcept override { return true; }

    class KnnPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<std::vector<Hit>> perSegHits;

    public:
      explicit KnnPreparedWeight(std::vector<std::vector<Hit>>&& perSegHits)
        : perSegHits(std::move(perSegHits)) {}

      Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) override {
        if ((size_t)segment.ord >= perSegHits.size()) return nullptr;
        auto& hits = perSegHits[(size_t)segment.ord];
        if (hits.empty()) return nullptr;
        return target.make<KnnQuery::Scorer>(
          std::span<const Hit>(hits.data(), hits.size()));
      }

      bool outputIsSubsetOfDomain() const noexcept override { return true; }
    };

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      IndexReader& reader = ctx.reader;
      size_t numSegs = reader.segments().size();
      std::vector<std::vector<Hit>> perSegHits(numSegs);
      if (vaux == nullptr) return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));

      faiss::Index* faissIdx = vaux->getFaissIndex();
      if (!faissIdx || faissIdx->ntotal == 0) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }
      // prepare() runs in the parallel phase, possibly deep in a work-stealing
      // stack; use the thread-local pool (inline buffer in TLS, not on this
      // frame) instead of a stack-resident MemPool.  Per-thread, so still off
      // the shared request pool.  Everything allocated here (prefix is heap;
      // the sparse selector / field-info live in this pool) is consumed within
      // prepare(), before the guard rewinds.
      auto poolGuard = MemPool::threadLocalPoolGuard();
      MemPool& scratch = poolGuard.pool();

      // Per-segment vector counts -> cumulative prefix for FAISS-id -> segment
      // mapping.  Builder added segments in ord order skipping zero-vector
      // ones, and our IndexInfo segments are in the same canonical order, so
      // the prefix sum lines up: zero-vector segments contribute a zero range.
      //
      // Within a segment, FAISS ids correspond to *value ranks* (0 ..
      // numVectors).  Each segment gets a SegV2D resolver mapping valueRank ->
      // docId: identity for dense single-valued, a materialized bitset lookup for
      // sparse single-valued, and the column's reverse map for multi-valued.
      std::vector<int64_t> prefix(numSegs + 1);
      prefix[0] = 0;
      std::vector<SegV2D> v2dPerSeg(numSegs);

      // Holds the multi-valued segments' valueRank->docId MonoReaders alive through
      // the FAISS search + result walk below.  MonoReader is
      // just pointers into the segment mmap (valid for the whole query), so copying it
      // out of the scratch VectorReader is safe and cheap.
      std::vector<std::optional<MonoReader>> monoHolders(numSegs);
      std::vector<SegFieldInfo> segInfoStorage(numSegs);
      std::vector<SegFieldInfo*> segInfos(numSegs, nullptr);
      for (size_t i = 0; i < numSegs; i++) {
        FieldReader fieldReader(scratch, reader.segments()[i].postingsReader());
        if (fieldReader.seek(query.getField())) {
          fieldReader.readFieldInfo(segInfoStorage[i]);
          segInfos[i] = &segInfoStorage[i];
        }
      }

      int64_t totalDocsWithValue = 0;
      bool anyMultiValued = false;
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = 0;
        if (segInfos[i] != nullptr) {
          // FIXED_SIZE vector column.  numVectors == #docs-with-field for
          // single-valued, or the total vector count for multi-valued.
          VectorReader vr(reader.segments()[i].postingsReader(), *segInfos[i]);
          segCount = vr.numVectors();
          if (segCount > 0) {
            totalDocsWithValue += vr.docsWithValue();
            if (vr.isMultiValued()) {
              anyMultiValued = true;
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
                v2dPerSeg[i].sel = makeValueDocSelector(scratch, dr.bitset());
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
      // normalized on write, or normalized by the builder for raw-column mode,
      // so we need a unit query for cosine = IP semantics to hold.  Done into a
      // local copy; the caller's span is unmodified.
      std::vector<float> queryBuf;
      const float* queryPtr = query.getQueryVec().data();
      if (vaux->getMetric() == proto::VectorParams::COSINE) {
        queryBuf.assign(query.getQueryVec().begin(), query.getQueryVec().end());
        faiss::fvec_renorm_L2((size_t)vaux->getDims(), 1, queryBuf.data());
        queryPtr = queryBuf.data();
      }

      // Use a faiss::IDSelector to filter deleted docs and the active query
      // domain *inside* the search, so over-requesting is used only for
      // multi-vector doc collapse.
      std::span<const int64_t> prefixSpan(prefix.data(), prefix.size());
      std::span<const SegV2D> v2dSpan(v2dPerSeg.data(), v2dPerSeg.size());
      DomainSelector selector(prefixSpan, v2dSpan, reader.segments(), ctx.domainPerSeg);
      faiss::SearchParameters params;
      params.sel = &selector;

      int64_t ntotal = faissIdx->ntotal;
      int64_t kDocs = query.getK();
      int64_t cap = std::min(ntotal, std::max((int64_t)1, maxKnnCandidates));
      int64_t avgMult = (anyMultiValued && totalDocsWithValue > 0)
        ? ceilDivClamped(ntotal, totalDocsWithValue, cap)
        : 1;
      int64_t kReq = mulClamped(kDocs, avgMult, cap);
      if (kReq <= 0) return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));

      BranchlessIndex<int64_t> segIndex(prefix.size());
      std::vector<DocHit> docHits;
      boost::unordered_flat_set<uint64_t> seenDocs;
      size_t maxDocHits = (size_t)std::min(kDocs, cap);
      docHits.reserve(maxDocHits);
      seenDocs.reserve(maxDocHits);
      FaissFlatVectorEngine engine(*faissIdx, params, prefixSpan, segIndex, vaux->getMetric());

      for (;;) {
        VectorSearchRequest request{
          .query = queryPtr,
          .dims = vaux->getDims(),
          .candidates = kReq,
        };
        VectorSearchResult result = engine.search(request);

        docHits.clear();
        seenDocs.clear();
        for (const auto& hit : result.hits) {
          int32_t docId = v2dPerSeg[(size_t)hit.segOrd].resolve(hit.valueRank);
          uint64_t key = packSegDoc(hit.segOrd, docId);
          if (seenDocs.insert(key).second) {
            // Flat results are already exact and sorted in score order, so the
            // first vector seen for this doc is its best vector.  Quantized
            // engines will rescore and sort before this collapse step.
            docHits.push_back({hit.segOrd, docId, hit.score});
            if ((int64_t)docHits.size() == kDocs) break;
          }
        }

        if ((int64_t)docHits.size() >= kDocs) break;
        if (result.poolExhausted && result.breadthExhausted) break;
        if (kReq >= cap) break;

        int64_t want = projectedKReq(kReq, kDocs, (int64_t)docHits.size(), cap);
        kReq = std::min(cap, std::max(kReq + 1, want));
      }

      if ((int64_t)docHits.size() < kDocs && cap < ntotal) {
        LOG_DEBUG("KnnQuery: {} of k={} docs for field '{}' at candidate cap {}",
                  docHits.size(), kDocs, query.getField(), cap);
      }

      // Materialize the per-segment hit arrays, sorted by docId.
      std::sort(docHits.begin(), docHits.end(), [](const DocHit& a, const DocHit& b) {
        return (a.segOrd == b.segOrd) ? (a.docId < b.docId) : (a.segOrd < b.segOrd);
      });
      for (size_t r = 0; r < docHits.size();) {
        int32_t ord = docHits[r].segOrd;
        size_t j = r + 1;
        while (j < docHits.size() && docHits[j].segOrd == ord) j++;
        auto& out = perSegHits[(size_t)ord];
        out.reserve(j - r);
        for (size_t i = r; i < j; i++) {
          out.push_back({docHits[i].docId, docHits[i].score});
        }
        r = j;
      }

      return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
    }

    Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) override {
      unused(target, segment);
      return nullptr;
    }

  private:
    class FaissFlatVectorEngine final : public VectorEngine {
      faiss::Index& index;
      faiss::SearchParameters& params;
      std::span<const int64_t> prefix;
      const BranchlessIndex<int64_t>& segIndex;
      int32_t metric;
      std::vector<faiss::idx_t> ids;
      std::vector<float> dists;

    public:
      FaissFlatVectorEngine(faiss::Index& index,
                            faiss::SearchParameters& params,
                            std::span<const int64_t> prefix,
                            const BranchlessIndex<int64_t>& segIndex,
                            int32_t metric) noexcept
        : index(index),
          params(params),
          prefix(prefix),
          segIndex(segIndex),
          metric(metric) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        assert(request.query != nullptr);
        assert(request.dims == index.d);

        ids.assign((size_t)request.candidates, (faiss::idx_t)-1);
        dists.assign((size_t)request.candidates, 0.0f);
        index.search(1, request.query, request.candidates, dists.data(), ids.data(), &params);

        VectorSearchResult result;
        result.hits.reserve((size_t)request.candidates);
        int64_t liveHits = 0;
        for (int64_t i = 0; i < request.candidates; i++) {
          faiss::idx_t fid = ids[(size_t)i];
          if (fid < 0) continue;  // FAISS pad - no more live results.
          liveHits++;
          int32_t ord = findSeg((int64_t)fid, prefix, segIndex);
          if (ord < 0) continue;
          int64_t valueRank = (int64_t)fid - prefix[(size_t)ord];
          result.hits.push_back({
            .score = scoreFromDist(dists[(size_t)i], metric),
            .segOrd = ord,
            .valueRank = valueRank,
          });
        }

        result.poolExhausted = liveHits < request.candidates || request.candidates >= index.ntotal;
        result.breadthExhausted = true;
        return result;
      }
    };
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

    int32_t advance(int32_t docid) override {
      if (cur >= 0 && cur < (int32_t)hits.size() && hits[cur].docId >= docid) {
        return hits[cur].docId;
      }
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }

    bool advanceExact(int32_t docid) override {
      return advance(docid) == docid;
    }

    int32_t docId() override {
      if (cur < 0) return -1;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    float score() override {
      assert(cur >= 0 && cur < (int32_t)hits.size());
      return hits[cur].score;
    }
  };

private:
  // Convert FAISS's per-metric distance to a "higher is better" score.
  //   L2:    FAISS returns squared distance - invert to 1/(1+d).
  //   IP:    FAISS returns dot product - already higher-is-better.
  //   COSINE: stored as IP over normalized vectors (write path or builder),
  //           query is normalized at search time - same as IP.
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
  static int32_t findSeg(int64_t faissId, std::span<const int64_t> prefix,
                         const BranchlessIndex<int64_t>& index) {
    if (faissId < 0 || faissId >= prefix.back()) return -1;
    auto it = index.upperBound(prefix.data(), faissId);
    return (int32_t)(it - prefix.data() - 1);
  }

  static uint64_t packSegDoc(int32_t segOrd, int32_t docId) {
    return ((uint64_t)(uint32_t)segOrd << 32) | (uint32_t)docId;
  }

  static int64_t ceilDivClamped(int64_t num, int64_t den, int64_t cap) {
    if (den <= 0) return cap;
    if (num <= 0) return 0;
    int64_t q = num / den;
    if (num % den != 0) q++;
    return std::min(q, cap);
  }

  static int64_t mulClamped(int64_t a, int64_t b, int64_t cap) {
    if (a <= 0 || b <= 0) return 0;
    if (a > cap / b) return cap;
    return std::min(a * b, cap);
  }

  static int64_t projectedKReq(int64_t kReq, int64_t kDocs, int64_t docsSeen, int64_t cap) {
    int64_t denom = std::max((int64_t)1, docsSeen);
    // Heuristic projection: kReq * (target docs / observed docs) plus slack.
    double want = std::ceil((double)kReq * (double)kDocs / (double)denom * 1.3);
    if (want >= (double)cap) return cap;
    return (int64_t)want;
  }

  /// faiss::IDSelector that maps a FAISS id to (segment ord, docRank) via
  /// the precomputed prefix sums + per-segment valueRank->docRank lookup,
  /// then consults the segment's liveDocs and the active query domain.
  /// FAISS calls is_member(id) for every candidate during search and skips
  /// those that return false.
  ///
  /// For IndexFlat this doesn't reduce the scan cost (we still touch every
  /// vector for distance computation) but it keeps deleted and out-of-domain
  /// vectors out of the returned candidate list.
  /// For HNSW/IVF later, the selector can also prune graph traversal / list
  /// selection.
  class DomainSelector : public faiss::IDSelector {
    std::span<const int64_t> prefix;
    // Precomputed monobound layout for prefix; is_member() runs per candidate
    // vector against this one array, so remembering it beats recomputing.
    BranchlessIndex<int64_t> segIndex;
    // Per-segment valueRank -> docId resolver (mono / flat / identity).
    std::span<const SegV2D> v2dPerSeg;
    std::span<IndexReader::Segment> segs;
    std::span<DocSet* const> domainPerSeg;

  public:
    DomainSelector(std::span<const int64_t> prefix,
                   std::span<const SegV2D> v2dPerSeg,
                   std::span<IndexReader::Segment> segs,
                   std::span<DocSet* const> domainPerSeg) noexcept
      : prefix(prefix), segIndex(prefix.size()), v2dPerSeg(v2dPerSeg),
        segs(segs), domainPerSeg(domainPerSeg) {}

    bool is_member(faiss::idx_t id) const final {
      int32_t ord = findSeg((int64_t)id, prefix, segIndex);
      if (ord < 0) return false;
      int64_t valueRank = (int64_t)id - prefix[ord];
      int32_t docId = v2dPerSeg[ord].resolve(valueRank);
      auto* live = segs[ord].liveDocs();
      if (live != nullptr && !live->bitset().get(docId)) return false;
      if (!domainPerSeg.empty()) {
        auto* domain = domainPerSeg[(size_t)ord];
        if (domain != nullptr && !domain->get(docId)) return false;
      }
      return true;
    }
  };
};

} // namespace solux
