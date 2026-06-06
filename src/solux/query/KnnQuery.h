#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <queue>
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
#include "solux/util/log.h"
#include "solux/util/screaming.h"

namespace solux {

/// kNN query against a vector field.
///
/// Execution model: kNN is globally bounded, so search materializes once during
/// the query preparation phase.  The default exact engine scans each segment's
/// vector column directly; a test / benchmark FAISS-flat aux engine remains,
/// and IVF+PQ aux indexes dispatch through the same seam.
/// Hits cross the seam as (segment, valueRank, score), then are collapsed to
/// docs and bucketed per segment in docId order for the segment-parallel
/// TopDocsReq pipeline.
///
/// The active query domain (liveDocs intersected with any enclosing filter
/// clauses) is pushed into the engine.  The column engine checks it directly
/// while scanning docs; FAISS aux engines use a segment-local IDSelector.
///
/// Multi-valued vector fields are supported via the segment's persisted
/// valueRank->docId column (VectorReader::docForVectorRank).  The exact column
/// engine collapses a doc's vectors while scanning and returns one best vector
/// per doc.  Approximate aux engines may return vector candidates instead; the
/// host loop unions, rescans exact scores when needed, sorts, and collapses
/// before publishing the global top-k docs.  Above maxKnnCandidates the result
/// is best-effort.
class KnnQuery final : public solux::Query {
  std::string_view field;
  const VectorFieldType& fieldType;
  std::span<const float> queryVec;
  int32_t k;
  int32_t nprobe;
  int32_t refineCandidates;
  bool exact;

public:
  /// One result hit within a segment.  docId is the local docRank within the
  /// segment; score is converted to "higher is better" regardless of vector
  /// metric (see scoreFromDist).
  struct Hit {
    int32_t docId;
    float score;
  };

  // Maximum engine candidates retained while trying to fill k distinct docs.
  // Exact column flat returns doc-collapsed candidates; aux engines may return
  // vector candidates.  Mutable so tests can shrink it for cap coverage.
  static inline int64_t maxKnnCandidates = 1000000;

  // Maximum recall breadth a non-flat engine can request while deepening.
  // Flat ignores breadth; this is a host safety cap for future engines.
  static inline int32_t maxKnnBreadth = 1000000;

  // Approximate ANN engines over-fetch vector candidates before exact
  // full-precision rescore.  Default sizing is affine with a DECAYING ratio:
  //   kReq = count + k * max(REFINE_MIN_RATIO, log2(REFINE_UNITY_K / k))
  // (same shape as Solr's distributed-faceting overrequest, count +
  // ratio*limit, with the ratio shrinking as k grows).  Rationale: PQ
  // mis-ranking displaces a true neighbor by an absolute number of impostors
  // that does not scale with k, so small k needs the fixed headroom - while
  // large k is shotgun retrieval whose near-tied tail nobody can rank
  // meaningfully (and no benchmark measures recall that deep), so the
  // multiplier decays to REFINE_MIN_RATIO by k = REFINE_UNITY_K instead of
  // inflating the rescore pool and the engine's result heap.  An explicit
  // request refine_candidates pins the pool size directly (clamped up to k -
  // an absolute count like nprobe, not a multiplier).  defaultAnnRefineRatio:
  // 0 = the adaptive decay; > 0 pins a fixed default ratio (tests use this
  // to emulate a pure multiplier).  Mutable for tests / benches.
  static inline int32_t defaultAnnRefineCount = 128;
  static inline int32_t defaultAnnRefineRatio = 0;
  static constexpr double REFINE_UNITY_K = 1000.0;
  static constexpr double REFINE_MIN_RATIO = 1.1;

  // Test seam: when set, prepare() wraps the production flat engine and runs
  // the host deepen loop against the wrapper instead.  Lets tests drive the
  // loop with non-flat engine behaviors (breadth rounds, approximate scores)
  // that no production engine exhibits yet.  Always null in production.
  static inline std::function<std::unique_ptr<VectorEngine>(VectorEngine& flat, int64_t ntotal)>
      engineWrapperForTests;

  /// Resolves a segment-local value rank to its owning docId.  Exactly one mode is
  /// active per segment:
  ///   - mono   : multi-valued reverse map (valueRank -> docId), read off the column.
  ///   - sel    : sparse single-valued (some docs lack the field) -> docId is the
  ///              valueRank-th set bit of the has-field bitset (select()).  No
  ///              per-vector array; the Selector caches only a small per-bucket
  ///              prefix and reuses the column's existing bitset.
  ///   - neither: dense single-valued, valueRank == docId (identity).
  /// The MonoReader / Selector must outlive every resolve() call (both live for the
  /// duration of the engine search + result walk inside Weight::prepare).
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

  struct VectorKey {
    int32_t segOrd;
    int64_t valueRank;
  };

  struct VectorKeyHash {
    size_t operator()(const VectorKey& key) const noexcept {
      uint64_t x = (uint64_t)(uint32_t)key.segOrd;
      uint64_t y = (uint64_t)key.valueRank;
      y ^= y >> 33;
      y *= 0xff51afd7ed558ccdULL;
      y ^= y >> 33;
      y *= 0xc4ceb9fe1a85ec53ULL;
      y ^= y >> 33;
      return (size_t)(y ^ (x * 0x9e3779b97f4a7c15ULL));
    }
  };

  struct VectorKeyEqual {
    bool operator()(const VectorKey& a, const VectorKey& b) const noexcept {
      return a.segOrd == b.segOrd && a.valueRank == b.valueRank;
    }
  };

  KnnQuery(std::string_view field, const VectorFieldType& fieldType,
           std::span<const float> queryVec, int32_t k,
           int32_t nprobe = 0, int32_t refineCandidates = 0, bool exact = false)
    : field(field), fieldType(fieldType), queryVec(queryVec), k(k),
      nprobe(nprobe), refineCandidates(refineCandidates), exact(exact) {}

  std::string_view getField() const noexcept { return field; }
  const VectorFieldType& getFieldType() const noexcept { return fieldType; }
  std::span<const float> getQueryVec() const noexcept { return queryVec; }
  int32_t getK() const noexcept { return k; }
  int32_t getNProbe() const noexcept { return nprobe; }
  int32_t getRefineCandidates() const noexcept { return refineCandidates; }
  bool getExact() const noexcept { return exact; }

  Query::Weight* createWeight(Query::Context& context) override {
    return context.pool.make<KnnQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    KnnQuery& query;

  public:
    Weight(Query::Context& context, KnnQuery& query)
      : Query::Weight(context), query(query) {
      if (!query.getFieldType().knnSearchable()) {
        throw std::runtime_error(std::format(
          "KnnQuery on vector field '{}' without a metric", query.getField()));
      }
      if (query.getFieldType().dims() > 0 &&
          (int32_t)query.getQueryVec().size() != query.getFieldType().dims()) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector dims {} do not match schema dims {} for field '{}'",
          query.getQueryVec().size(), query.getFieldType().dims(), query.getField()));
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

    struct EngineSlot {
      std::unique_ptr<VectorEngine> engine;
      int64_t vectorCount = 0;
    };

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      IndexReader& reader = ctx.reader;
      size_t numSegs = reader.segments().size();
      std::vector<std::vector<Hit>> perSegHits(numSegs);

      // prepare() runs in the parallel phase, possibly deep in a work-stealing
      // stack; use the thread-local pool (inline buffer in TLS, not on this
      // frame) instead of a stack-resident MemPool.  Per-thread, so still off
      // the shared request pool.  Sparse selectors / field-info live in this
      // pool and are consumed within prepare(), before the guard rewinds.
      auto poolGuard = MemPool::threadLocalPoolGuard();
      MemPool& scratch = poolGuard.pool();

      // Within a segment, FAISS ids are segment-local value ranks (0 ..
      // numVectors).  Each segment gets a SegV2D resolver mapping valueRank to
      // docId: identity for dense single-valued, a bitset select() lookup for
      // sparse single-valued, and the column's reverse map for multi-valued.
      std::vector<SegV2D> v2dPerSeg(numSegs);
      std::vector<int64_t> segVectorCounts(numSegs, 0);
      std::vector<VectorAuxReader*> vauxPerSeg(numSegs, nullptr);
      std::string auxName("vec.");
      auxName.append(query.getField());
      if (!query.getExact()) {
        for (size_t i = 0; i < numSegs; i++) {
          auto aux = reader.segments()[i].getAuxReader(auxName);
          if (!aux) continue;
          auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
          if (!vaux) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment overlay '{}' is not a vector index (kind={})",
              auxName, aux->getKind()));
          }
          vauxPerSeg[i] = vaux;
        }
      }

      // Holds the multi-valued segments' valueRank->docId MonoReaders alive through
      // the engine search + result walk below.  MonoReader is
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
      int32_t dims = query.getFieldType().dims() > 0
        ? query.getFieldType().dims()
        : (int32_t)query.getQueryVec().size();
      if (dims <= 0) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector for field '{}' must have positive dims", query.getField()));
      }
      int32_t metric = (int32_t)query.getFieldType().metric();
      bool normalizeColumnOnCosineRescore =
        query.getFieldType().metric() == VectorFieldType::METRIC_COSINE
        && !query.getFieldType().normalized()
        && !query.getFieldType().normalizeOnWrite();
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = 0;
        if (segInfos[i] != nullptr) {
          // FIXED_SIZE vector column.  numVectors == #docs-with-field for
          // single-valued, or the total vector count for multi-valued.
          VectorReader vr(reader.segments()[i].postingsReader(), *segInfos[i]);
          segCount = vr.numVectors();
          if (segCount > 0) {
            if (vr.dims() != dims) {
              throw std::runtime_error(std::format(
                "KnnQuery: query vector dims {} do not match segment dims {} for field '{}'",
                dims, vr.dims(), query.getField()));
            }
            totalDocsWithValue += vr.docsWithValue();
            segVectorCounts[i] = segCount;
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
      }
      int64_t ntotal = 0;
      for (int64_t n : segVectorCounts) ntotal += n;
      if (ntotal == 0) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }

      // Optionally normalize the query for COSINE - stored vectors were
      // normalized on write, or normalized by the builder for raw-column mode,
      // so we need a unit query for cosine = IP semantics to hold.  Done into a
      // local copy; the caller's span is unmodified.
      std::vector<float> queryBuf;
      const float* queryPtr = query.getQueryVec().data();
      if (metric == proto::VectorParams::COSINE) {
        queryBuf.assign(query.getQueryVec().begin(), query.getQueryVec().end());
        faiss::fvec_renorm_L2((size_t)dims, 1, queryBuf.data());
        queryPtr = queryBuf.data();
      }

      std::span<const SegV2D> v2dSpan(v2dPerSeg.data(), v2dPerSeg.size());

      int64_t kDocs = query.getK();
      int64_t cap = std::min(ntotal, std::max((int64_t)1, maxKnnCandidates));
      // exact is a contract: the maxKnnCandidates host heuristic must not
      // silently truncate the requested k (the explicit knob wins).  ntotal
      // still bounds it - fewer stored docs than k is not a violation, the
      // true top-k is simply all of them.
      if (query.getExact()) cap = std::min(ntotal, std::max(cap, kDocs));
      std::vector<EngineSlot> slots;
      slots.reserve(numSegs);
      bool approximateEngine = false;
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = segVectorCounts[i];
        if (segCount <= 0 || segInfos[i] == nullptr) continue;
        VectorAuxReader* vaux = vauxPerSeg[i];
        if (vaux != nullptr) {
          if (vaux->getDims() != dims) {
            throw std::runtime_error(std::format(
              "KnnQuery: query vector dims {} do not match segment index dims {} for field '{}'",
              dims, vaux->getDims(), query.getField()));
          }
          if (vaux->getMetric() != metric) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment index metric {} does not match field metric {} for field '{}'",
              vaux->getMetric(), metric, query.getField()));
          }
          normalizeColumnOnCosineRescore =
            normalizeColumnOnCosineRescore || vaux->shouldNormalizeColumnOnCosineRescore();
          faiss::Index* idx = vaux->getFaissIndex();
          if (idx == nullptr || idx->ntotal == 0) continue;
          if (idx->ntotal != segCount) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment FAISS ntotal={} != segment vector count={} for field '{}'",
              idx->ntotal, segCount, query.getField()));
          }
          bool isIvf = vaux->getEngine() == VectorAuxMeta::ENGINE_IVFPQ;
          approximateEngine = approximateEngine || !vaux->scoresAreExact();
          slots.push_back({
            .engine = std::make_unique<SegmentFaissVectorEngine>(
              *idx, (int32_t)i, reader.segments()[i], v2dPerSeg[i],
              ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i],
              metric, vaux->scoresAreExact(), isIvf,
              vaux->getNList(), vaux->getDefaultBreadth()),
            .vectorCount = segCount,
          });
        } else {
          slots.push_back({
            .engine = std::make_unique<SegmentFlatColumnVectorEngine>(
              (int32_t)i, reader.segments()[i], segInfos[i], v2dPerSeg[i],
              ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i],
              dims, metric, normalizeColumnOnCosineRescore),
            .vectorCount = segCount,
          });
        }
      }
      if (slots.empty()) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }

      int64_t avgMult = (approximateEngine && anyMultiValued && totalDocsWithValue > 0)
        ? ceilDivClamped(ntotal, totalDocsWithValue, cap)
        : 1;
      int64_t baseReq = kDocs;
      if (approximateEngine) {
        // Explicit refine_candidates pins the candidate pool size (an
        // absolute count, clamped up to k); the absent-knob default is
        // affine with a k-decaying ratio (see defaultAnnRefineCount above).
        if (query.getRefineCandidates() > 0) {
          baseReq = std::min(cap, std::max(kDocs, (int64_t)query.getRefineCandidates()));
        } else {
          double ratio = defaultAnnRefineRatio > 0
            ? (double)defaultAnnRefineRatio
            : std::max(REFINE_MIN_RATIO, std::log2(REFINE_UNITY_K / (double)kDocs));
          baseReq = std::min(cap, (int64_t)defaultAnnRefineCount
                                    + (int64_t)((double)kDocs * ratio));
        }
        baseReq = std::max(baseReq, kDocs);
      }
      int64_t kReq = mulClamped(baseReq, avgMult, cap);
      if (kReq <= 0) return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));

      std::vector<VectorEngineHit> candidateHits;
      boost::unordered_flat_set<VectorKey, VectorKeyHash, VectorKeyEqual> seenVectors;
      std::vector<DocHit> docHits;
      boost::unordered_flat_set<uint64_t> seenDocs;
      size_t maxDocHits = (size_t)std::min(kDocs, cap);
      size_t maxVectorHits = (size_t)cap;
      candidateHits.reserve((size_t)std::min(kReq, cap));
      seenVectors.reserve((size_t)std::min(kReq, cap));
      docHits.reserve(maxDocHits);
      seenDocs.reserve(maxDocHits);

      std::unique_ptr<VectorEngine> baseEngine =
        std::make_unique<CompositeVectorEngine>(std::move(slots), ntotal, kDocs);
      VectorEngine* engine = baseEngine.get();
      std::unique_ptr<VectorEngine> wrapperEngine;
      if (engineWrapperForTests) {
        wrapperEngine = engineWrapperForTests(*baseEngine, ntotal);
        engine = wrapperEngine.get();
      }

      int32_t breadth = query.getNProbe() > 0 ? query.getNProbe() : 0;
      int32_t maxBreadth = query.getNProbe() > 0
        ? query.getNProbe()
        : std::max(0, maxKnnBreadth);
      bool candidatePoolSorted = true;
      for (;;) {
        VectorSearchRequest request{
          .query = queryPtr,
          .dims = dims,
          .candidates = kReq,
          .breadth = breadth,
        };
        VectorSearchResult result = engine->search(request);

        size_t appendStart = candidateHits.size();
        for (const auto& hit : result.hits) {
          if (candidateHits.size() >= maxVectorHits) break;
          VectorKey key{hit.segOrd, hit.valueRank};
          if (seenVectors.insert(key).second) {
            candidateHits.push_back(hit);
          }
        }
        bool appended = candidateHits.size() != appendStart;
        if (appended) {
          candidatePoolSorted = false;
        }

        // Per-segment engines can append hits that interleave with the
        // existing cross-segment pool.  Approximate engines must rescore
        // appended hits before sorting / doc collapse.
        if (!result.scoresAreExact && appended) {
          rescoreCandidates(std::span<VectorEngineHit>(candidateHits.data() + appendStart,
                                                       candidateHits.size() - appendStart),
                            reader,
                            std::span<SegFieldInfo* const>(segInfos.data(), segInfos.size()),
                            queryPtr, dims, metric, normalizeColumnOnCosineRescore);
          candidatePoolSorted = false;
        }
        if (!candidatePoolSorted) {
          sortCandidatesByScore(candidateHits);
          candidatePoolSorted = true;
        }
        collapseCandidates(candidateHits, v2dSpan, kDocs, docHits, seenDocs);

        if ((int64_t)docHits.size() >= kDocs) break;
        if (candidateHits.size() >= maxVectorHits) break;

        if (!result.poolExhausted && kReq < cap) {
          int64_t want = projectedKReq(kReq, kDocs, (int64_t)docHits.size(), cap);
          kReq = std::min(cap, std::max(kReq + 1, want));
          continue;
        }

        if (result.poolExhausted && !result.breadthExhausted && breadth < maxBreadth) {
          int32_t nextBreadth = result.nextBreadth > breadth ? result.nextBreadth : breadth + 1;
          breadth = std::min(nextBreadth, maxBreadth);
          continue;
        }

        break;
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
    class SegmentFaissVectorEngine final : public VectorEngine {
      class LocalDomainSelector final : public faiss::IDSelector {
        int32_t segOrd;
        IndexReader::Segment& seg;
        const SegV2D& v2d;
        DocSet* domain;

      public:
        LocalDomainSelector(int32_t segOrd, IndexReader::Segment& seg,
                            const SegV2D& v2d, DocSet* domain) noexcept
          : segOrd(segOrd), seg(seg), v2d(v2d), domain(domain) {}

        bool is_member(faiss::idx_t id) const final {
          if (id < 0) return false;
          int32_t docId = v2d.resolve((int64_t)id);
          auto* live = seg.liveDocs();
          if (live != nullptr && !live->bitset().get(docId)) return false;
          if (domain != nullptr && !domain->get(docId)) return false;
          unused(segOrd);
          return true;
        }
      };

      faiss::Index& index;
      int32_t segOrd;
      LocalDomainSelector selector;
      int32_t metric;
      bool exactScores;
      bool isIvf;
      int32_t nlist;
      int32_t defaultBreadth;
      std::vector<faiss::idx_t> ids;
      std::vector<float> dists;

    public:
      SegmentFaissVectorEngine(faiss::Index& index,
                               int32_t segOrd,
                               IndexReader::Segment& seg,
                               const SegV2D& v2d,
                               DocSet* domain,
                               int32_t metric,
                               bool exactScores,
                               bool isIvf,
                               int32_t nlist,
                               int32_t defaultBreadth) noexcept
        : index(index),
          segOrd(segOrd),
          selector(segOrd, seg, v2d, domain),
          metric(metric),
          exactScores(exactScores),
          isIvf(isIvf),
          nlist(nlist),
          defaultBreadth(defaultBreadth) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        assert(request.query != nullptr);
        assert(request.dims == index.d);
        if (request.candidates <= 0) {
          VectorSearchResult empty;
          empty.scoresAreExact = exactScores;
          empty.poolExhausted = true;
          empty.breadthExhausted = true;
          return empty;
        }

        ids.assign((size_t)request.candidates, (faiss::idx_t)-1);
        dists.assign((size_t)request.candidates, 0.0f);

        faiss::SearchParameters flatParams;
        flatParams.sel = &selector;
        faiss::SearchParametersIVF ivfParams;
        ivfParams.sel = &selector;
        // ivfParams.max_codes stays 0 (unlimited) by design: entries within an
        // IVF list are insertion-ordered, not relevance-ordered, so any
        // within-list truncation drops arbitrary candidates - possibly the
        // best one.  Breadth (nprobe) is the only legitimate work limiter;
        // probed lists are always scanned in full.

        faiss::SearchParameters* params = &flatParams;
        int32_t effectiveBreadth = 0;
        if (isIvf) {
          assert(nlist > 0);
          // INTERIM breadth rule for Milestone 1: the request nprobe applies
          // to every segment and is clamped to that segment's nlist.  With no
          // explicit nprobe, use this segment's sqrt(nlist) build default.
          // This is the known equal-work trap; Milestone 2 replaces it with a
          // pinned total-effort budget and global list allocation.
          int32_t fallback = defaultBreadth > 0
            ? defaultBreadth
            : std::max<int32_t>(1, (int32_t)std::sqrt((double)nlist));
          effectiveBreadth = request.breadth > 0 ? request.breadth : fallback;
          effectiveBreadth = std::clamp(effectiveBreadth, 1, std::max(1, nlist));
          ivfParams.nprobe = (size_t)effectiveBreadth;
          params = &ivfParams;
        }

        index.search(1, request.query, request.candidates, dists.data(), ids.data(), params);

        VectorSearchResult result;
        result.hits.reserve((size_t)request.candidates);
        int64_t liveHits = 0;
        for (int64_t i = 0; i < request.candidates; i++) {
          faiss::idx_t fid = ids[(size_t)i];
          if (fid < 0) continue;  // FAISS pad - no more live results.
          liveHits++;
          result.hits.push_back({
            .score = scoreFromDist(dists[(size_t)i], metric),
            .segOrd = segOrd,
            .valueRank = (int64_t)fid,
          });
        }

        result.scoresAreExact = exactScores;
        result.poolExhausted = liveHits < request.candidates || request.candidates >= index.ntotal;
        result.breadthExhausted = nlist <= 0 || effectiveBreadth >= nlist;
        if (!result.breadthExhausted) {
          result.nextBreadth = std::min(nlist, std::max(effectiveBreadth + 1, effectiveBreadth * 2));
        }
        return result;
      }
    };

    class SegmentFlatColumnVectorEngine final : public VectorEngine {
      int32_t segOrd;
      IndexReader::Segment& seg;
      SegFieldInfo* segInfo;
      const SegV2D& v2d;
      DocSet* domain;
      int32_t dims;
      int32_t metric;
      bool normalizeColumnOnCosineRescore;

      static bool better(const VectorEngineHit& a, const VectorEngineHit& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
        return a.valueRank < b.valueRank;
      }

      struct WorstFirst {
        bool operator()(const VectorEngineHit& a, const VectorEngineHit& b) const {
          return better(a, b);
        }
      };

    public:
      SegmentFlatColumnVectorEngine(int32_t segOrd,
                                    IndexReader::Segment& seg,
                                    SegFieldInfo* segInfo,
                                    const SegV2D& v2d,
                                    DocSet* domain,
                                    int32_t dims,
                                    int32_t metric,
                                    bool normalizeColumnOnCosineRescore) noexcept
        : segOrd(segOrd),
          seg(seg),
          segInfo(segInfo),
          v2d(v2d),
          domain(domain),
          dims(dims),
          metric(metric),
          normalizeColumnOnCosineRescore(normalizeColumnOnCosineRescore) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        assert(request.query != nullptr);
        assert(request.dims == dims);

        VectorSearchResult result;
        result.scoresAreExact = true;
        result.breadthExhausted = true;
        if (request.candidates <= 0) {
          result.poolExhausted = true;
          return result;
        }

        std::priority_queue<VectorEngineHit, std::vector<VectorEngineHit>, WorstFirst> heap;
        int64_t eligibleDocs = 0;
        if (segInfo == nullptr) {
          result.poolExhausted = true;
          return result;
        }
        VectorReader vr(seg.postingsReader(), *segInfo);
        int64_t numVals = vr.numVectors();
        if (numVals == 0) {
          result.poolExhausted = true;
          return result;
        }
        if (vr.dims() != dims) {
          throw std::runtime_error(std::format(
            "KnnQuery: query vector dims {} do not match segment dims {}",
            dims, vr.dims()));
        }

        // Inline run-collapse: value ranks are doc-ordered (the valDoc map is
        // monotonic; sparse select and dense identity likewise), so a doc's
        // vectors form a contiguous run and max-sim collapse falls out of one
        // pass.  liveDocs/domain are applied as a per-run predicate.
        if (v2d.mono != nullptr) {
          MonoReader::BulkValues valDoc(*v2d.mono);
          int64_t idx = valDoc.next();
          int64_t rank = 0;
          while (rank < numVals) {
            int32_t docId = (int32_t)valDoc.value();
            int64_t end = rank;
            do {
              end++;
              idx = valDoc.next();
            } while (end < numVals && (int32_t)valDoc.value() == docId);
            unused(idx);

            if (docEligible(docId)) {
              eligibleDocs++;
              VectorEngineHit best{
                .score = 0.0f,
                .segOrd = segOrd,
                .valueRank = rank,
              };
              bool haveScore = false;
              for (int64_t r = rank; r < end; r++) {
                float score = exactScore(request.query, vr.vectorAtRank(r), dims, metric,
                                         normalizeColumnOnCosineRescore, 0.0f);
                if (!haveScore || score > best.score) {
                  best.score = score;
                  best.valueRank = r;
                  haveScore = true;
                }
              }
              offer(heap, best, request.candidates);
            }
            rank = end;
          }
        } else {
          // Single-valued (dense identity or sparse select): runs are length
          // 1 by construction.
          for (int64_t rank = 0; rank < numVals; rank++) {
            int32_t docId = v2d.resolve(rank);
            if (!docEligible(docId)) continue;
            eligibleDocs++;
            offer(heap,
                  VectorEngineHit{
                    .score = exactScore(request.query, vr.vectorAtRank(rank), dims, metric,
                                        normalizeColumnOnCosineRescore, 0.0f),
                    .segOrd = segOrd,
                    .valueRank = rank,
                  },
                  request.candidates);
          }
        }

        result.hits.reserve(heap.size());
        while (!heap.empty()) {
          result.hits.push_back(heap.top());
          heap.pop();
        }
        std::sort(result.hits.begin(), result.hits.end(), better);
        result.poolExhausted = eligibleDocs <= request.candidates;
        return result;
      }

    private:
      bool docEligible(int32_t docId) const {
        auto* live = seg.liveDocs();
        if (live != nullptr && !live->bitset().get(docId)) return false;
        if (domain != nullptr && !domain->get(docId)) return false;
        return true;
      }

      static void offer(
          std::priority_queue<VectorEngineHit, std::vector<VectorEngineHit>, WorstFirst>& heap,
          const VectorEngineHit& hit,
          int64_t limit) {
        if ((int64_t)heap.size() < limit) {
          heap.push(hit);
          return;
        }
        if (!heap.empty() && better(hit, heap.top())) {
          heap.pop();
          heap.push(hit);
        }
      }
    };

    class CompositeVectorEngine final : public VectorEngine {
      std::vector<EngineSlot> slots;
      int64_t totalVectors;
      int64_t kDocs;

      static bool better(const VectorEngineHit& a, const VectorEngineHit& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
        return a.valueRank < b.valueRank;
      }

      int64_t candidatesForSlot(int64_t requested, int64_t n) const {
        if (requested <= 0 || n <= 0) return 0;
        // INTERIM depth rule for Milestone 1: distribute by segment size and
        // floor each segment at min(k, N_i).  Milestone 2 owns global
        // allocation and total-effort semantics.
        long double proportional = ((long double)requested * (long double)n)
          / (long double)std::max<int64_t>(1, totalVectors);
        int64_t bySize = (int64_t)std::ceil(proportional);
        int64_t floor = std::min(kDocs, n);
        return std::min(n, std::max(bySize, floor));
      }

    public:
      CompositeVectorEngine(std::vector<EngineSlot>&& slots, int64_t totalVectors, int64_t kDocs) noexcept
        : slots(std::move(slots)), totalVectors(totalVectors), kDocs(kDocs) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        VectorSearchResult merged;
        merged.scoresAreExact = true;
        merged.poolExhausted = true;
        merged.breadthExhausted = true;
        if (request.candidates <= 0) return merged;

        for (auto& slot : slots) {
          int64_t candidates = candidatesForSlot(request.candidates, slot.vectorCount);
          if (candidates <= 0) continue;
          VectorSearchRequest subReq = request;
          subReq.candidates = candidates;
          VectorSearchResult part = slot.engine->search(subReq);
          merged.scoresAreExact = merged.scoresAreExact && part.scoresAreExact;
          merged.poolExhausted = merged.poolExhausted && part.poolExhausted;
          merged.breadthExhausted = merged.breadthExhausted && part.breadthExhausted;
          if (!part.breadthExhausted && part.nextBreadth > request.breadth) {
            if (merged.nextBreadth == 0) {
              merged.nextBreadth = part.nextBreadth;
            } else {
              merged.nextBreadth = std::min(merged.nextBreadth, part.nextBreadth);
            }
          }
          merged.hits.insert(merged.hits.end(),
                             std::make_move_iterator(part.hits.begin()),
                             std::make_move_iterator(part.hits.end()));
        }
        std::stable_sort(merged.hits.begin(), merged.hits.end(), better);
        return merged;
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
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  static void rescoreCandidates(std::span<VectorEngineHit> candidates,
                                IndexReader& reader,
                                std::span<SegFieldInfo* const> segInfos,
                                const float* queryPtr,
                                int32_t dims,
                                int32_t metric,
                                bool normalizeColumnOnCosineRescore) {
    std::vector<std::optional<VectorReader>> vectorReaders(segInfos.size());
    for (auto& candidate : candidates) {
      if (candidate.segOrd < 0 || (size_t)candidate.segOrd >= segInfos.size()) {
        throw std::runtime_error("KnnQuery: vector candidate segment ordinal out of range");
      }
      SegFieldInfo* info = segInfos[(size_t)candidate.segOrd];
      if (info == nullptr) {
        throw std::runtime_error("KnnQuery: vector candidate segment has no field info");
      }
      auto& vr = vectorReaders[(size_t)candidate.segOrd];
      if (!vr) {
        vr.emplace(reader.segments()[(size_t)candidate.segOrd].postingsReader(), *info);
      }
      candidate.score = exactScore(queryPtr, vr->vectorAtRank(candidate.valueRank),
                                   dims, metric, normalizeColumnOnCosineRescore,
                                   candidate.score);
    }
  }

  // Keep L2/IP conversions in sync with scoreFromDist below.  COSINE has a
  // separate stored-vector norm branch for raw-column rescore mode.
  static float exactScore(const float* queryPtr,
                          std::span<const float> vec,
                          int32_t dims,
                          int32_t metric,
                          bool normalizeColumnOnCosineRescore,
                          float fallbackScore) {
    assert(queryPtr != nullptr);
    assert((int32_t)vec.size() == dims);
    switch (metric) {
      case proto::VectorParams::L2: {
        float dist = faiss::fvec_L2sqr(queryPtr, vec.data(), (size_t)dims);
        return 1.0f / (1.0f + dist);
      }
      case proto::VectorParams::IP:
        return faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims);
      case proto::VectorParams::COSINE: {
        float sum = faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims);
        if (!normalizeColumnOnCosineRescore) return sum;

        // Raw-column cosine pays the vector norm at query time.  This is the
        // explicit tradeoff for preserving the originally submitted bytes when
        // normalize_on_write=false; the default cosine path stores unit vectors.
        float normSq = faiss::fvec_norm_L2sqr(vec.data(), (size_t)dims);
        if (!std::isfinite(normSq) || normSq <= MIN_COSINE_NORM_SQ) return 0.0f;
        return sum / std::sqrt(normSq);
      }
      default:
        return fallbackScore;
    }
  }

  static void sortCandidatesByScore(std::vector<VectorEngineHit>& candidates) {
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const VectorEngineHit& a, const VectorEngineHit& b) {
          return a.score > b.score;
        });
  }

  static void collapseCandidates(const std::vector<VectorEngineHit>& candidates,
                                 std::span<const SegV2D> v2dPerSeg,
                                 int64_t kDocs,
                                 std::vector<DocHit>& docHits,
                                 boost::unordered_flat_set<uint64_t>& seenDocs) {
    docHits.clear();
    seenDocs.clear();
    for (const auto& hit : candidates) {
      int32_t docId = v2dPerSeg[(size_t)hit.segOrd].resolve(hit.valueRank);
      uint64_t key = packSegDoc(hit.segOrd, docId);
      if (seenDocs.insert(key).second) {
        docHits.push_back({hit.segOrd, docId, hit.score});
        if ((int64_t)docHits.size() == kDocs) break;
      }
    }
  }

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
        return dist;
    }
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

};

} // namespace solux
