#include <algorithm>
#include <charconv>
#include <latch>

#include <tbb/task_group.h>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"

using namespace solux;
using namespace solux::test;


namespace solux {
void buildBenchIndex(CollectionHelper& helper, int64_t nDocs, std::span<const int32_t> docsPerSeg) {
  unused(nDocs);
  helper.clear();

  std::vector<std::pair<int32_t, int32_t>> vals;

  auto iw = helper.getIndexWriter();
  
  // Pre-obtain all inverters we need for parallel segment building
  std::vector<Inverter*> inverters;
  inverters.reserve(docsPerSeg.size());
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    inverters.push_back(&iw->obtainInverter());
  }

  // Calculate starting document ID for each segment
  std::vector<int64_t> segmentStartIds;
  segmentStartIds.reserve(docsPerSeg.size());
  int64_t idNum = 0;
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    segmentStartIds.push_back(idNum);
    idNum += docsPerSeg[i];
  }

  // Build all segments in parallel using TBB task_group
  tbb::task_group tg;
  for (size_t segnum = 0; segnum < docsPerSeg.size(); segnum++) {
    tg.run([&, segnum]() {
      int segDocs = docsPerSeg[segnum];
      Inverter& inverter = *inverters[segnum];
      int64_t localIdNum = segmentStartIds[segnum];
      
      Inverter::IndexHandler& s0 = inverter.getIndexHandler("id");
      Inverter::IndexHandler& s1 = inverter.getIndexHandler("short_u10_s");
      Inverter::IndexHandler& s2 = inverter.getIndexHandler("short_u10k_s");
      Inverter::IndexHandler& s3 = inverter.getIndexHandler("short_u100k_s");
      Inverter::IndexHandler& s4 = inverter.getIndexHandler("short_u1m_s");
      Inverter::IndexHandler& s5 = inverter.getIndexHandler("med_u10_s");
      Inverter::IndexHandler& s6 = inverter.getIndexHandler("med_u10k_s");
      Inverter::IndexHandler& s7 = inverter.getIndexHandler("med_u1m_s");
      // Present on ~1% of docs: the sparse-field shape every other string field
      // here lacks.  Doc-driven counting costs O(domain) no matter how few docs
      // hold a value, so this is where term-driven counting can pay off.
      Inverter::IndexHandler& s8 = inverter.getIndexHandler("sparse_u1k_s");
      // 100 uniform values, so one term selects ~1% of docs: the query-driven
      // 1%-selectivity posture (a per-term density no other field here has).
      Inverter::IndexHandler& s9 = inverter.getIndexHandler("short_u100_s");
      // 1000 uniform values: ~0.1%/term, the sparse query-driven tier.
      Inverter::IndexHandler& s10 = inverter.getIndexHandler("short_u1000_s");
      Inverter::IndexHandler& i1 = inverter.getIndexHandler("u10_i");
      Inverter::IndexHandler& i2 = inverter.getIndexHandler("u10k_i");
      Inverter::IndexHandler& i3 = inverter.getIndexHandler("u10m_i");
      // Mostly-single-valued multi-valued int: the only field here that makes
      // a column carry an endValueRank mono sidecar, which every multi-valued
      // read consults twice per doc.  1/64 of docs hold a second value, so the
      // value ranks are a near-unit ramp.
      Inverter::IndexHandler& i4 = inverter.getIndexHandler("u10_is");

      std::string s;
      for (int i = 0; i < segDocs; i++) {
        SplitMix64 r(localIdNum); // make each doc predictable

        // add some random values to the index
        inverter.startDoc();
        s0.index(inverter, std::to_string(localIdNum++));

        s1.index(inverter, std::to_string(r.rint(10)));
        s2.index(inverter, std::to_string(r.rint(10000)));
        s3.index(inverter, std::to_string(r.rint(100000)));
        s4.index(inverter, std::to_string(r.rint(1000000)));

        s.resize(0);
        s.append(std::to_string(r.rint(10)));
        s.append("medium_length_string_no_SSO");
        s5.index(inverter, s);

        s.resize(0);
        s.append(std::to_string(r.rint(100)));
        s.append("medium_length_string_no_SSO");
        s.append(std::to_string(r.rint(100)));
        s6.index(inverter, s);

        s.resize(0);
        s.append(std::to_string(r.rint(1000)));
        s.append("medium_length_string_no_SSO");
        s.append(std::to_string(r.rint(1000)));
        s7.index(inverter, s);

        if (r.rint(100) == 0) {
          s8.index(inverter, std::to_string(r.rint(1000)));
        }

        // Drawn from an independent stream so inserting this field leaves
        // every other field's per-doc values (and bench fingerprints) intact.
        SplitMix64 r2(localIdNum ^ 0x9E3779B97F4A7C15ULL);
        s9.index(inverter, std::to_string(r2.rint(100)));
        s10.index(inverter, std::to_string(r2.rint(1000)));

        i1.index(inverter, r.rint(10));

        auto iVal = r.rint(10000);
        i2.index(inverter, iVal);

        i3.index(inverter, r.rint(10000000));

        // One call carrying every value: the multi-valued handler records the
        // doc once per call, so calling it per value would add the doc to a
        // term's stream twice.  The second value is drawn from the other 9 so
        // a doc never lands in one facet bucket twice, and they go in
        // ascending, the natural order for a multi-valued column.
        int64_t isVals[2] = {r.rint(10), 0};
        size_t nIsVals = 1;
        if (r.rint(64) == 0) {
          int64_t other = r.rint(9);
          if (other >= isVals[0]) other++;
          isVals[1] = other;
          if (isVals[1] < isVals[0]) std::swap(isVals[0], isVals[1]);
          nIsVals = 2;
        }
        i4.index(inverter, std::span<const int64_t>(isVals, nIsVals));

        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);  // immediately request a flush of the segment.
    });
  }
  tg.wait();

  helper.commit();

  if (!solux::unit_tests) {
    malloc_trim(0);
    std::println(std::cerr,"Post buildBenchIndex - Peak RSS: {} KB, current RSS: KB {}", peakRSSKB(), currentRSSKB());
  }
}
}

static void BM_QueryBuildIndex(benchmark::State& state, int64_t nDocs, std::string_view shape) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = SoluxTest::scaleTestWork(200);
  }

  test::CollectionHelper helper;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  for (auto _ : state) {
    buildBenchIndex(helper, nDocs, docsPerSeg);
    benchmark::ClobberMemory();
  }

  // test that index was built correctly
  auto reader = helper.getIndexWriter()->getIndexReader();
  int readerSegs = reader->segments().size();
  ASSERT_EQ(readerSegs, docsPerSeg.size());
  // check each segment size
  for (size_t i=0; i<docsPerSeg.size(); i++) {
    auto& seg = reader->segments()[i];
    ASSERT_EQ(seg.postingsReader().maxDoc(), docsPerSeg[i]);
  }


  auto indexWriter = helper.getIndexWriter();
  auto& dir = indexWriter->dir;
  std::vector<std::string> files;
  dir.listFiles(files);
  // std::println(std::cout, "Index files: {}", files);
  // calculate the total size of the index
  int64_t totalSize = 0;
  for (const auto& file : files) {
    auto f = dir.openFile(file);
    totalSize += f->size();
  }
  // std::println(std::cout, "Total index size: {} bytes", totalSize);

  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
  state.counters["nSegs"] = readerSegs; // number of segments in the index
  state.counters["indexSz"] = totalSize; // size of the index in bytes
}


int64_t scanIntCol(std::string_view field, IndexReader& reader) {
  int64_t count = 0;
  for (auto& seg : reader.segments()) {
    // create a fieldReader and look up the field
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(seg.postingsReader());
    bool hasField = fieldReader.seek(field);
    if (!hasField) {
      continue;
    }
    // read the SegFieldInfo
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    IntColReader colReader(seg.postingsReader(), fieldInfo);
    IntColReader::Iterator iter(colReader);
    while (true) {
      int32_t doc = iter.next();
      if (doc == IntColReader::ENDDOC) {
        break;
      }
      auto val = iter.value();
      if (val > 0) {
        count++;
      }
    }
  }
  return count;
}

//
// TODO: optionally go through grpc for searching to see how much overhead that adds.
//
static void BM_Query(benchmark::State& state, int64_t nDocs, std::string_view shape, std::string_view qfield, std::string_view sfield, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  std::vector<std::string_view> sortFields;
  for (size_t start = 0; start <= sfield.size();) {
    size_t end = sfield.find(',', start);
    sortFields.push_back(sfield.substr(start, end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }

  if (solux::unit_tests) {
    nDocs = SoluxTest::scaleTestWork(200);
  }

  //
  // Figure out how many docs in each segment we want.
  //
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    // build the index if it doesn't exist or is not correct.
    buildBenchIndex(helper, nDocs, docsPerSeg);
  }

  // if the sortfield ends in _s, we want to make sure to pre-load the OrdMap
  std::shared_ptr<OrdMap> ordMap;
  bool hasStringSort = std::ranges::any_of(
    sortFields, [](std::string_view field) { return field.ends_with("_s"); });
  if (hasStringSort) {
    auto reader = helper.getIndexWriter()->getIndexReader();
    /* basic info for the ordMap
    ordMap = reader->getOrdMap(sfield);
    for (int seg = 0; seg < reader->segments().size(); seg++) {
      auto segToGlob = ordMap ? ordMap->getSegToGlobal(seg) : OrdMap::SegToGlobal();
      println(std::cout, "OrdMap for segment {} ndocs={} nOrds={} deltas={}", seg, reader->segments()[seg].maxDoc(), segToGlob.numOrds, (void*)segToGlob.deltas);
    }
    */
  }

  RSSWatcher watcher;

  int64_t fp = -1;
  for (auto _ : state) {
    int64_t ret = 0;

    // scan int col just to get an idea of what a full scan costs vs all the sort logic.
    if (qfield=="scan") {
      fp = scanIntCol(sfield, *helper.getIndexWriter()->getIndexReader());
      continue;
    }

    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("main");
    // match all docs query
    auto& topDocs = req->topDocs("q");

    if (qfield == "all") {
      topDocs.allQuery();
    } else {
      topDocs.matchQuery(qfield, "0");
    }
    topDocs.limit(100)        // higher limit to exercise the priority queues more
           .getNumber(true)
           .getScores(false)
           .fields({"id"});
    for (auto field : sortFields) qb::sort(topDocs, field, qb::DESC);

    req->execute(para);

    // Now lets fingerprint the results to make sure we get the same every time.
    const auto* docs = req->responses[0]->proto.ops.at("q")->docList();
    // convert the ids back to integers and add them up.
    const auto& idCol = std::get<solux::api::ColStr>(docs->columns.at("id").kind);
    // start with the number of matches
    ret += docs->found.value_or(0);
    for (int i = 0; i < (int)idCol.v.size(); i++) {
      int64_t id = 0;
      std::from_chars(idCol.v[i].data(), idCol.v[i].data() + idCol.v[i].size(), id);
      ret = ret * 31 + id;
    }

    benchmark::DoNotOptimize(ret);

    if (fp != -1) {
      ASSERT_EQ(fp, ret); // sanity check that we get the same result every time.
    }
    fp = ret;  // save the fingerprint for the next iteration
  }

  state.counters["fp"] = fp;  // sanity check.
  state.counters["reused"] = reuseIndex;; // did we reuse the index?
  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
  // LOG_ERROR("fingerprint={}", fp); // verified is exactly the same on lucene
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
  if (hasStringSort) {
    state.counters["OrdMapSz"] = ordMap ? (double)ordMap->sizeInBytes() : 0; // size of the OrdMap in bytes
  }
}



// Conjunction (AND of two terms) -- the canonical advance()/leapfrog workload:
// the smaller posting list leads and advance()s the larger across doc blocks,
// which is exactly what postings skip data accelerates.  Kept deliberately lean
// (count all matches, no sort, no scores) so the conjunction iteration -- not
// the collector -- dominates, making the skip-data before/after visible.
static void BM_QueryConj(benchmark::State& state, int64_t nDocs, std::string_view shape,
                         std::string_view field1, std::string_view field2, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = SoluxTest::scaleTestWork(200);
  }

  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildBenchIndex(helper, nDocs, docsPerSeg);
  }

  int64_t fp = -1;
  for (auto _ : state) {
    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q");

    // field1:0 AND field2:0 (both required/scoring).  Clause order is irrelevant;
    // BooleanQuery leads with the lower-docfreq clause and advance()s the other.
    topDocs.rawQuery() = qb::boolean(topDocs.mr(),
        /*required=*/{qb::match(topDocs.mr(), field1, "0"),
                      qb::match(topDocs.mr(), field2, "0")});
    topDocs.limit(10)
           .getNumber(true)    // count all matches -> iterate the full conjunction
           .getScores(false);

    req->execute(para);

    const auto* docs = req->responses[0]->proto.ops.at("q")->docList();
    int64_t ret = docs->found.value_or(0);
    benchmark::DoNotOptimize(ret);

    if (fp != -1) {
      ASSERT_EQ(fp, ret);  // same match count every iteration
    }
    fp = ret;
  }

  state.counters["matches"] = fp;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}


// When we test sparse sets for performance, the most interesting case is when it's still a bitset in the block.
// Search code will spend much less time in very sparse sets.
constexpr int32_t nDocs = 10'000'000; // nocommit
constexpr const char* shape = "9555"; // 9 segments, 555 docs per segment

SOLUX_BENCHMARK_CAPTURE(BM_QueryBuildIndex, build,              nDocs, shape);

// use this one for profiling...
// SOLUX_BENCHMARK_CAPTURE(BM_Query, u10_i,            nDocs, shape, "all", "u10_i", false)->MinTime(30);
// #ifdef REMOVED
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10_i_scan,        nDocs, shape, "scan","u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10_i,             nDocs, shape, "all", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10_i_score,       nDocs, shape, "all", "u10_i,_score_", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10_i_para,        nDocs, shape, "all", "u10_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10k_i,            nDocs, shape, "all", "u10k_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10k_i_para,       nDocs, shape, "all", "u10k_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10m_i,            nDocs, shape, "all", "u10m_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, u10m_i_para,       nDocs, shape, "all", "u10m_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u10_s,      nDocs, shape, "all", "short_u10_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u10_s_para, nDocs, shape, "all", "short_u10_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u10k_s,      nDocs, shape, "all", "short_u10k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u10k_s_para, nDocs, shape, "all", "short_u10k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u100k_s,      nDocs, shape, "all", "short_u100k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u100k_s_para, nDocs, shape, "all", "short_u100k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u1m_s,      nDocs, shape, "all", "short_u1m_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Query, short_u1m_s_para, nDocs, shape, "all", "short_u1m_s", true);

// Conjunction shapes (rare term leads, advance()s the common one across blocks).
SOLUX_BENCHMARK_CAPTURE(BM_QueryConj, conj_dense_sparse,      nDocs, shape, "short_u10_s",  "short_u100k_s", false);  // ~1M AND ~100
SOLUX_BENCHMARK_CAPTURE(BM_QueryConj, conj_dense_sparse_para, nDocs, shape, "short_u10_s",  "short_u100k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_QueryConj, conj_dense_mid,         nDocs, shape, "short_u10_s",  "short_u10k_s",  false);  // ~1M AND ~1k
SOLUX_BENCHMARK_CAPTURE(BM_QueryConj, conj_mid_sparse,        nDocs, shape, "short_u10k_s", "short_u100k_s", false);  // ~1k AND ~100
// #endif
