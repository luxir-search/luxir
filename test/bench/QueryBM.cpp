#include <charconv>
#include <latch>

#include <tbb/task_group.h>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

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
      Inverter::IndexHandler& i1 = inverter.getIndexHandler("u10_i");
      Inverter::IndexHandler& i2 = inverter.getIndexHandler("u10k_i");
      Inverter::IndexHandler& i3 = inverter.getIndexHandler("u10m_i");

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

        i1.index(inverter, r.rint(10));

        auto iVal = r.rint(10000);
        i2.index(inverter, iVal);

        i3.index(inverter, r.rint(10000000));

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
    nDocs = 200;
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
    FieldReader fieldReader(poolGuard.pool(), seg.postingsReader());
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

  if (solux::unit_tests) {
    nDocs = 200;
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
  if (sfield.ends_with("_s")) {
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

    auto* lreq = LocalReq::create(SoluxTest::soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");
    // match all docs query
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();

    if (qfield == "all") {
      topDocs.mutable_query()->set_all(true);
    } else {
      auto& matchQuery = *topDocs.mutable_query()->mutable_match();
      matchQuery.set_field(qfield);
      matchQuery.mutable_val()->set_s("0");
    }
    topDocs.set_limit(100); // higher limit to exercise the priority queues more
    topDocs.set_get_number(true);
    topDocs.set_get_scores(false);
    topDocs.add_fields("id");
    // sort by some of the string fields
    auto* sortSpec = topDocs.add_sorts();
    sortSpec->set_field(sfield);
    sortSpec->set_dir(proto::SortSpec::DESC);

    lreq->engine.submit(*lreq, para);

    // Now lets fingerprint the results to make sure we get the same every time.
    const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
    // convert the ids back to integers and add them up.
    const auto& idCol = docs.columns().at("id").col_s();
    // start with the number of matches
    ret += docs.matches();
    for (int i = 0; i < idCol.v_size(); i++) {
      int64_t id = 0;
      std::from_chars(idCol.v(i).data(), idCol.v(i).data() + idCol.v(i).size(), id);
      ret = ret * 31 + id;
    }

    lreq->done();
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
  if (sfield.ends_with("_s")) {
    state.counters["OrdMapSz"] = ordMap ? (double)ordMap->sizeInBytes() : 0; // size of the OrdMap in bytes
  }
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
// #endif
