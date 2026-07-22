#include <algorithm>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "bench/solux_bench.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/index/TrieBuilder.h"
#include "solux/query/FuzzyQuery.h"
#include "solux/query/Query.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/FuzzySeekEnum.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/search/Collector.h"
#include "solux/store/Directory.h"
#include "test/SegmentTest.h"

using namespace solux;

namespace {

constexpr int32_t kUnitTerms = Postings::TERMS_BLOCK_SIZE * 20;
constexpr int32_t kBenchTerms = 1000000;
constexpr int32_t kCeilJumpBlocks = 7;
constexpr std::string_view kFieldName = "term_seek_bench_s";
constexpr std::string_view kStatsFieldName = "term_seek_bench_w";
constexpr std::string_view kDictPath = "/usr/share/dict/american-english";
constexpr int32_t kDenseTargetTerms = 256;
constexpr int32_t kDenseExactDocsPerTerm = 100;
constexpr int32_t kDenseVariantDocsPerTerm = 175;
constexpr int32_t kDenseVariantTerms = 2;
constexpr int32_t kStatsMaxDocs = 300000;

// Term corpus shapes.  DECIMAL is synthetic and regular (deep, narrow,
// digit-only fanout - the id-like shape).  WORDS is a real English
// vocabulary (natural letter fanout, the term-query shape, dictionary
// sized).  COMPOUND is word_word pairs from that vocabulary - natural
// prefix structure scaled to ~1M distinct terms (the shape of shingle /
// compound / multi-source dictionaries).
enum class TermSource { DECIMAL, WORDS, COMPOUND };

std::string makeBenchTerm(int32_t ord) {
  std::string term = "term_00000000";
  for (int32_t i = (int32_t)term.size() - 1; i >= 5; i--) {
    term[i] = (char)('0' + (ord % 10));
    ord /= 10;
  }
  return term;
}

const std::vector<std::string>& dictWords() {
  static std::vector<std::string> words = [] {
    std::vector<std::string> out;
    std::ifstream in{std::string(kDictPath)};
    std::string line;
    while (std::getline(in, line)) {
      bool plain = !line.empty();
      for (char c : line) {
        if (c < 'a' || c > 'z') { plain = false; break; }
      }
      if (plain) out.push_back(line);
    }
    // all-ASCII lowercase, so default string ordering == unsigned byte order
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }();
  return words;
}

int64_t rootNodeSize(const char* node) {
  uint8_t header = (uint8_t)node[0];
  uint8_t sign = header & 3;
  if (sign == TrieBuilder::SIGN_LEAF) return 1;

  int64_t deltaWidth = (int64_t)(((header >> 3) & 7) + 1);
  if (sign == TrieBuilder::SIGN_SINGLE) {
    return 2 + deltaWidth + TrieBuilder::ORD_WIDTH;
  }

  assert(sign == TrieBuilder::SIGN_MULTI);
  int64_t strategyBytes = (int64_t)((uint8_t)node[2] + 1);
  uint8_t minLabel = (uint8_t)node[1];
  uint8_t strategy = header >> 6;
  int64_t childCount = 0;

  if (strategy == TrieBuilder::STRATEGY_BITS) {
    const char* bitmap = node + 3;
    for (int64_t i = 0; i < strategyBytes; i++) {
      childCount += (int64_t)std::popcount((uint32_t)(uint8_t)bitmap[i]);
    }
  } else if (strategy == TrieBuilder::STRATEGY_ARRAY) {
    childCount = strategyBytes + 1;
  } else {
    assert(strategy == TrieBuilder::STRATEGY_REVERSE_ARRAY);
    uint8_t maxLabel = (uint8_t)node[3];
    int64_t absentCount = strategyBytes - 1;
    childCount = (int64_t)((uint32_t)maxLabel - (uint32_t)minLabel + 1) - absentCount;
  }

  return 3 + strategyBytes + childCount * (deltaWidth + TrieBuilder::ORD_WIDTH);
}

int64_t trieRegionBytes(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) {
  InputStream trieIS = postingsReader.getInputStreamSeek(fieldInfo.trieLoc);
  const char* base = trieIS.ptr();
  const char* root = base + fieldInfo.trieRootOff;
  return fieldInfo.trieRootOff + rootNodeSize(root) + 8;
}

void appendBenchToken(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

std::string denseBenchBody(std::string_view term, int32_t docOrd) {
  std::string body;
  int32_t tf = 0;
  int32_t filler = 0;
  if (docOrd < 16) {
    tf = 96;
    filler = 0;
  } else {
    tf = 1 + (docOrd % 7);
    filler = 24 + (docOrd % 37);
  }
  appendBenchToken(body, term, tf);
  appendBenchToken(body, "zzbenchfiller", filler);
  if ((docOrd & 3) == 0) {
    appendBenchToken(body, "yybenchpad", 3);
  }
  return body;
}

std::string denseVariantBenchBody(std::string_view term, int32_t docOrd) {
  std::string body;
  int32_t tf = 1;
  int32_t filler = 96 + (docOrd % 17);
  appendBenchToken(body, term, tf);
  appendBenchToken(body, "zzbenchfiller", filler);
  if ((docOrd & 3) == 0) {
    appendBenchToken(body, "yybenchpad", 3);
  }
  return body;
}

std::string denseVariantTerm(std::string_view term, int32_t variant) {
  assert(term.size() > 1);
  std::string ret(term);
  int32_t pos = 1 + (variant % ((int32_t) ret.size() - 1));
  ret[(size_t) pos] = (char)('0' + (variant % 10));
  return ret;
}

class TermSeekCorpus {
public:
  SegmentTest seg;
  RAMDir queryDir;
  std::unique_ptr<IndexReader> queryReader;
  SegFieldInfo fieldInfo;
  std::vector<std::string> terms;
  std::vector<int32_t> hitOrds;
  std::vector<int32_t> denseOrds;
  std::vector<std::string> missTargets;
  std::vector<std::string> ceilTargets;
  std::vector<int32_t> ceilExpectedOrds;
  int32_t nTerms;
  uint64_t seed;
  TermSource source;
  int32_t nBlocks = 0;
  int64_t termsRegionBytes = 0;
  int64_t termBlockOffsetBytes = 0;
  int64_t trieBytes = 0;

  TermSeekCorpus(TermSource sourceIn, int32_t termCount, uint64_t seedIn)
      : nTerms(termCount), seed(seedIn), source(sourceIn) {
    build();
  }

  IndexReader& statsReader() {
    if (queryReader == nullptr) {
      buildStatsReader();
    }
    return *queryReader;
  }

private:
  void buildTerms() {
    if (source == TermSource::DECIMAL) {
      terms.reserve(nTerms);
      for (int32_t i = 0; i < nTerms; i++) {
        terms.push_back(makeBenchTerm(i));
      }
      return;
    }

    const std::vector<std::string>& words = dictWords();
    ASSERT_FALSE(words.empty());

    if (source == TermSource::WORDS) {
      // real vocabulary; unit mode strides down to a small subset
      int32_t stride = std::max((int32_t)1, (int32_t)(words.size() / (size_t)nTerms));
      for (size_t i = 0; i < words.size(); i += (size_t)stride) {
        terms.push_back(words[i]);
      }
    } else {
      // word_word compounds; deterministic pair choice, dupes removed below
      // (birthday collisions ~100 at 1M pairs from ~64K^2 candidates).
      // NOTE: both indices must not be functions of i mod w or the pair
      // sequence repeats with period w - draw both from one SplitMix64.
      size_t w = words.size();
      SplitMix64 r(seed);
      terms.reserve(nTerms);
      for (int32_t i = 0; i < nTerms; i++) {
        uint64_t h = r();
        const std::string& a = words[(size_t)(h % w)];
        const std::string& b = words[(size_t)((h >> 32) % w)];
        terms.push_back(a + "_" + b);
      }
      std::sort(terms.begin(), terms.end());
      terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    }
    nTerms = (int32_t)terms.size();
  }

  void buildDenseOrds() {
    if (source != TermSource::WORDS) {
      return;
    }

    int32_t extraDocsPerDense = kDenseExactDocsPerTerm
        + kDenseVariantTerms * kDenseVariantDocsPerTerm - 1;
    int32_t maxDenseByDocs = extraDocsPerDense > 0 && kStatsMaxDocs > nTerms
        ? (kStatsMaxDocs - nTerms) / extraDocsPerDense
        : 0;
    int32_t target = std::min({kDenseTargetTerms, maxDenseByDocs, nTerms});
    if (target <= 0) {
      return;
    }

    std::vector<uint8_t> selected((size_t) nTerms, 0);
    int32_t stride = std::max(1, nTerms / target);
    auto addDenseOrd = [&](int32_t ord) {
      if ((int32_t)terms[(size_t)ord].size() < 5 || selected[(size_t)ord]) {
        return;
      }
      selected[(size_t)ord] = 1;
      denseOrds.push_back(ord);
    };
    for (int32_t ord = 0; ord < nTerms && (int32_t)denseOrds.size() < target; ord += stride) {
      addDenseOrd(ord);
    }
    for (int32_t ord = 0; ord < nTerms && (int32_t)denseOrds.size() < target; ord++) {
      addDenseOrd(ord);
    }
  }

  void buildTargets() {
    hitOrds.resize(nTerms);
    std::iota(hitOrds.begin(), hitOrds.end(), 0);
    std::mt19937_64 rng(seed);
    std::shuffle(hitOrds.begin(), hitOrds.end(), rng);

    missTargets.reserve(nTerms);
    for (int32_t ord : hitOrds) {
      std::string target = terms[ord];
      target.push_back('~');
      missTargets.push_back(std::move(target));
    }

    int32_t jump = Postings::TERMS_BLOCK_SIZE * kCeilJumpBlocks;
    for (int32_t ord = 0; ord + 1 < nTerms; ord += jump) {
      std::string target = terms[ord];
      target.push_back('~');
      // '~' sorts after every lowercase extension, so the ceiling can be
      // several terms past ord+1 in a real vocabulary; compute it.
      auto it = std::lower_bound(terms.begin(), terms.end(), target);
      if (it == terms.end()) break;
      ceilExpectedOrds.push_back((int32_t)(it - terms.begin()));
      ceilTargets.push_back(std::move(target));
    }
  }

  void writeIndex() {
    seg.initWriter();
    TextWriter writer(*seg.postingsWriter.get());
    writer.startField(std::string(kFieldName));
    for (int32_t ord = 0; ord < nTerms; ord++) {
      TermRef term(seg.pool, terms[ord].data(), (uint32_t)terms[ord].size());
      writer.startTerm(term);
      writer.startDoc(ord);
      writer.addPositionDelta(1);
      writer.endDoc(ord);
      writer.endTerm(term);
    }
    writer.endField();
  }

  void readFieldInfo() {
    seg.initReader();
    ASSERT_TRUE(seg.fieldReader->readNextField());
    ASSERT_EQ((std::string_view)kFieldName, seg.fieldReader->name());
    seg.fieldReader->readFieldInfo(fieldInfo);
    ASSERT_EQ(nTerms, fieldInfo.nTerms);
  }

  void buildStatsReader() {
    IndexWriter writer(queryDir);
    Inverter& inverter = writer.obtainInverter();
    auto& handler = inverter.getIndexHandler(kStatsFieldName);
    std::vector<uint8_t> dense((size_t)nTerms, 0);
    for (int32_t ord : denseOrds) {
      dense[(size_t)ord] = 1;
    }
    int32_t docid = 0;
    for (int32_t ord = 0; ord < nTerms; ord++) {
      std::string_view term = terms[(size_t)ord];
      if (dense[(size_t)ord]) {
        for (int32_t docOrd = 0; docOrd < kDenseExactDocsPerTerm; docOrd++) {
          inverter.setDoc(docid++);
          std::string body = denseBenchBody(term, docOrd);
          handler.index(inverter, std::string_view(body));
        }
        for (int32_t variant = 0; variant < kDenseVariantTerms; variant++) {
          std::string variantTerm = denseVariantTerm(term, variant);
          for (int32_t docOrd = 0; docOrd < kDenseVariantDocsPerTerm; docOrd++) {
            inverter.setDoc(docid++);
            std::string body = denseVariantBenchBody(variantTerm, docOrd);
            handler.index(inverter, std::string_view(body));
          }
        }
      } else {
        inverter.setDoc(docid++);
        handler.index(inverter, term);
      }
    }
    if (source == TermSource::WORDS) {
      ASSERT_LE(docid, kStatsMaxDocs);
    }
    writer.releaseInverter(inverter);
    writer.commit();
    queryReader = std::make_unique<IndexReader>(queryDir);
    ASSERT_EQ(queryReader->segments().size(), 1u);
  }

  void build() {
    buildTerms();
    buildDenseOrds();
    buildTargets();
    writeIndex();
    readFieldInfo();

    nBlocks = ((fieldInfo.nTerms - 1) / Postings::TERMS_BLOCK_SIZE) + 1;
    termsRegionBytes = fieldInfo.termBlockIndexLoc.offset() - fieldInfo.termsLoc.offset();
    termBlockOffsetBytes = (int64_t)nBlocks * (int64_t)sizeof(int64_t);
    trieBytes = trieRegionBytes(*seg.reader, fieldInfo);
  }
};

std::shared_ptr<TermSeekCorpus> getTermSeekCorpus(TermSource source) {
  if (source != TermSource::DECIMAL && dictWords().empty()) {
    return nullptr;  // no system word list on this machine; caller skips
  }
  int32_t nTerms = solux::unit_tests ? kUnitTerms : kBenchTerms;
  uint64_t seed = solux::unit_tests ? SoluxTest::rng_seed : 0x51f15eeda5c0ffeeull;

  static std::shared_ptr<TermSeekCorpus> corpus;
  if (corpus == nullptr || corpus->source != source || corpus->seed != seed
      || (source == TermSource::DECIMAL && corpus->nTerms != nTerms)) {
    corpus = std::make_shared<TermSeekCorpus>(source, nTerms, seed);
  }
  return corpus;
}

void addSizingCounters(benchmark::State& state, const TermSeekCorpus& corpus) {
  state.counters["nTerms"] = corpus.nTerms;
  state.counters["nBlocks"] = corpus.nBlocks;
  state.counters["denseBaseTerms"] = corpus.denseOrds.size();
  state.counters["denseTerms"] = corpus.denseOrds.size() * (1 + kDenseVariantTerms);
  state.counters["termsRegionBytes"] = corpus.termsRegionBytes;
  state.counters["termBlockOffsetsBytes"] = corpus.termBlockOffsetBytes;
  state.counters["trieBytes"] = corpus.trieBytes;
}

uint64_t addStatsFingerprint(uint64_t fp, const Similarity::TermStats& stats) {
  fp = fp * 31 + (uint64_t)stats.docFreq;
  fp = fp * 31 + (uint64_t)stats.totalTermFreq;
  return fp;
}

bool lookupTermStatsPostings(MemPool& pool, IndexReader& reader, CachedFieldInfo& cachedFieldInfo,
                             std::string_view term, Similarity::TermStats& result) {
  result = {};
  bool found = false;
  for (int i = 0; i < (int)reader.segments().size(); ++i) {
    TermsEnum* termsEnum = cachedFieldInfo.termsEnums[i];
    if (termsEnum == nullptr || !termsEnum->seek(term)) {
      continue;
    }
    found = true;
    DocsOnlyEnum docsEnum(*termsEnum);
    result.docFreq += docsEnum.numDocs();
    result.totalTermFreq += docsEnum.totalTermFreq();
  }
  return found;
}

int64_t countScoredFuzzyMatches(IndexReader& reader, std::string_view q) {
  MemPool pool;
  Query::Context context(pool, reader);
  FuzzyQuery fuzzy(kStatsFieldName, q, 1, 1);
  auto* weight = fuzzy.createWeight(context, Query::NEED_SCORES);
  int64_t total = 0;

  auto segments = context.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t)segments.size(); segnum++) {
    Query::Scorer* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) continue;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      total++;
    }
  }

  return total;
}

} // namespace

static void BM_TermSeekExact_hit(benchmark::State& state, TermSource source) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  MemPool pool;
  TermsEnum tenum(pool, *corpus->seg.reader, corpus->fieldInfo);

  uint64_t fingerprint = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    for (int32_t ord : corpus->hitOrds) {
      std::string_view target = corpus->terms[ord];
      bool found = tenum.seek(target);
      assert(found);
      assert(tenum.ord() == ord);
      fp = fp * 31 + (uint64_t)tenum.ord();
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)corpus->hitOrds.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

static void BM_TermSeekExact_miss(benchmark::State& state, TermSource source) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  MemPool pool;
  TermsEnum tenum(pool, *corpus->seg.reader, corpus->fieldInfo);

  uint64_t fingerprint = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    for (const std::string& target : corpus->missTargets) {
      bool found = tenum.seek(target);
      assert(!found);
      fp = fp * 31 + 7;
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)corpus->missTargets.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

static void BM_TermSeekCeil_jump(benchmark::State& state, TermSource source) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  MemPool pool;
  TermsEnum tenum(pool, *corpus->seg.reader, corpus->fieldInfo);

  uint64_t fingerprint = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    for (int32_t i = 0; i < (int32_t)corpus->ceilTargets.size(); i++) {
      bool found = tenum.seekCeil(corpus->ceilTargets[i]);
      assert(found);
      assert(tenum.ord() == corpus->ceilExpectedOrds[i]);
      fp = fp * 31 + (uint64_t)tenum.ord();
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)corpus->ceilTargets.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

static void BM_TermStats_dict(benchmark::State& state, TermSource source) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  IndexReader& reader = corpus->statsReader();
  MemPool pool;
  Query::Context context(pool, reader);
  CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(kStatsFieldName);
  ASSERT_NE(cachedFieldInfo, nullptr);

  uint64_t fingerprint = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    for (int32_t ord : corpus->hitOrds) {
      Similarity::TermStats stats;
      bool found = context.lookupTermStats(*cachedFieldInfo, corpus->terms[(size_t)ord], stats);
      assert(found);
      fp = addStatsFingerprint(fp, stats);
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)corpus->hitOrds.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

static void BM_TermStats_postings(benchmark::State& state, TermSource source) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  IndexReader& reader = corpus->statsReader();
  MemPool pool;
  Query::Context context(pool, reader);
  CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(kStatsFieldName);
  ASSERT_NE(cachedFieldInfo, nullptr);

  uint64_t fingerprint = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    for (int32_t ord : corpus->hitOrds) {
      std::string_view term = corpus->terms[(size_t)ord];
      Similarity::TermStats stats;
      bool found = lookupTermStatsPostings(pool, reader, *cachedFieldInfo, term, stats);
      assert(found);
      if (solux::unit_tests) {
        Similarity::TermStats dictStats;
        bool dictFound = context.lookupTermStats(*cachedFieldInfo, term, dictStats);
        ASSERT_EQ(dictFound, found);
        ASSERT_EQ(dictStats.docFreq, stats.docFreq);
        ASSERT_EQ(dictStats.totalTermFreq, stats.totalTermFreq);
      }
      fp = addStatsFingerprint(fp, stats);
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)corpus->hitOrds.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

// Whole-enum fuzzy matching: prefix_length=0 (jump-heavy - every prefix stays
// alive), whole query as the automaton suffix, enumerate ALL matching terms.
// This is the smart-seek enum's end-to-end cost, which seekBlock is only one
// component of.
static void BM_FuzzyEnum(benchmark::State& state, TermSource source, int maxEdits) {
  auto corpus = getTermSeekCorpus(source);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;

  std::vector<std::string> queries;
  int32_t stride = std::max(1, corpus->nTerms / 64);
  for (int32_t ord = 0; ord < corpus->nTerms; ord += stride) {
    queries.push_back(corpus->terms[(size_t)ord]);
  }

  uint64_t fingerprint = 0;
  int64_t matched = 0;
  int64_t jumps = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    matched = 0;
    jumps = 0;
    for (const std::string& q : queries) {
      MemPool pool;
      TermsEnum tenum(pool, *corpus->seg.reader, corpus->fieldInfo);
      FuzzySeekEnum fe(pool, tenum, "", q, maxEdits);
      while (fe.next()) {
        fp = fp * 31 + (uint64_t)fe.terms().ord();
        matched++;
      }
      jumps += fe.jumps();
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)queries.size());
  addSizingCounters(state, *corpus);
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
  state.counters["matched"] = matched;
  state.counters["jumps"] = jumps;
}

static void BM_FuzzyTopK(benchmark::State& state) {
  auto corpus = getTermSeekCorpus(TermSource::WORDS);
  if (skipBenchIfDataMissing(state, corpus != nullptr, kDictPath)) return;
  IndexReader& reader = corpus->statsReader();

  std::vector<std::string> queries;
  int32_t queryTarget = solux::unit_tests ? 8 : 64;
  int32_t stride = std::max(1, (int32_t)corpus->denseOrds.size() / queryTarget);
  for (int32_t i = 0; i < (int32_t)corpus->denseOrds.size()
       && (int32_t)queries.size() < queryTarget; i += stride) {
    queries.push_back(corpus->terms[(size_t)corpus->denseOrds[(size_t)i]]);
  }
  if (queries.empty()) {
    state.SkipWithMessage("no dense words for fuzzy top-k");
    return;
  }

  int64_t totalMatches = 0;
  for (const std::string& q : queries) {
    totalMatches += countScoredFuzzyMatches(reader, q);
  }

  uint64_t fingerprint = 0;
  int64_t visited = 0;
  for (auto _ : state) {
    uint64_t fp = 1;
    visited = 0;
    for (const std::string& q : queries) {
      MemPool pool;
      Query::Context context(pool, reader);
      FuzzyQuery fuzzy(kStatsFieldName, q, 1, 1);
      auto* weight = fuzzy.createWeight(context, Query::NEED_SCORES);
      TopDocsCollector collector(10);
      MaxScoreAccumulator accumulator;

      auto segments = context.topReader.segments();
      for (int32_t segnum = 0; segnum < (int32_t)segments.size(); segnum++) {
        auto* scorer = weight->createScorer(pool, segments[segnum]);
        if (scorer != nullptr) {
          collectTopK(segnum, scorer, nullptr, nullptr, collector, true, &accumulator);
        }
      }

      visited += collector.totalHits();
      auto topDocs = collector.sort();
      fp = fp * 31 + (uint64_t)collector.totalHits();
      for (const auto& sd : topDocs) {
        fp = fp * 31 + (uint64_t)sd.doc.docId();
        fp = fp * 31 + (uint64_t)sd.doc.segment();
        fp = fp * 31 + (uint64_t)std::bit_cast<uint32_t>(sd.score);
      }
    }
    benchmark::DoNotOptimize(fp);
    fingerprint = fp;
  }

  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)queries.size());
  addSizingCounters(state, *corpus);
  state.counters["queries"] = queries.size();
  state.counters["visited"] = visited;
  state.counters["totalMatches"] = totalMatches;
  state.counters["visitedRatio"] = totalMatches > 0
      ? (double) visited / (double) totalMatches
      : 0.0;
  state.counters["fp"] = (int64_t)(fingerprint % 100000);
}

// grouped by source so the single-slot corpus cache builds each corpus once
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_hit, decimal, TermSource::DECIMAL);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_miss, decimal, TermSource::DECIMAL);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekCeil_jump, decimal, TermSource::DECIMAL);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_dict, decimal, TermSource::DECIMAL);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_postings, decimal, TermSource::DECIMAL);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_hit, words, TermSource::WORDS);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_miss, words, TermSource::WORDS);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekCeil_jump, words, TermSource::WORDS);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_dict, words, TermSource::WORDS);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_postings, words, TermSource::WORDS);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_hit, compound, TermSource::COMPOUND);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekExact_miss, compound, TermSource::COMPOUND);
SOLUX_BENCHMARK_CAPTURE(BM_TermSeekCeil_jump, compound, TermSource::COMPOUND);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_dict, compound, TermSource::COMPOUND);
SOLUX_BENCHMARK_CAPTURE(BM_TermStats_postings, compound, TermSource::COMPOUND);
SOLUX_BENCHMARK_CAPTURE(BM_FuzzyEnum, compound_e2, TermSource::COMPOUND, 2);
SOLUX_BENCHMARK_CAPTURE(BM_FuzzyEnum, words_e1, TermSource::WORDS, 1);
SOLUX_BENCHMARK_CAPTURE(BM_FuzzyEnum, words_e2, TermSource::WORDS, 2);
SOLUX_BENCHMARK(BM_FuzzyTopK);
// about 1.2% slower when not ommitting frame pointer
// adding term hashes (without using them) resulted in a slowdown of ~1%
// med is about 5% slower than small (before any optimizations like using hashes or pulling out prefixes from block starts)
//

static void BM_TermFind(benchmark::State& state, uint64_t maxId, int hitPercent) {
  int nPosPerDoc=1;
  // int nTerms=1000000;
  int nTerms=1000000;
  uint64_t seed = 1;
  if (solux::unit_tests) {
    nTerms = SoluxTest::rng.rint(1, Postings::TERMS_BLOCK_SIZE*10);
    seed = SoluxTest::rng_seed;
  }
  auto fname = "myfield";

  SegmentTest seg;
  seg.initWriter();
  MemPool& pool = seg.pool;

  std::unordered_set<int64_t> idSet(nTerms*2);  // used to ensure terms are unique... TODO: replace with something better like flat hash map
  std::vector<PackedTerm> terms;
  terms.reserve(nTerms*2);
  SplitMix64 r(seed);
  std::string termStr;
  termStr.resize(21);
  while ((int)terms.size() < nTerms*2) {   // generate twice as many terms, but only index the first half
    int64_t id = (r() & 0x7fffffffffffffff) % maxId;
    auto [iter, inserted] = idSet.insert(id);
    if (!inserted) continue; // a repeat
    auto [end, ec] = std::to_chars(termStr.data(), termStr.data() + 21, id);
    terms.emplace_back(seg.pool, termStr.data(), end-termStr.data());
  }

  // indirectly sort the terms so we can use the index as the docid
  std::vector<int> sorted(nTerms);
  std::iota( std::begin(sorted), std::end(sorted), 0 );
  std::sort( std::begin(sorted), std::end(sorted),
             [&terms] (int i, int j) { return terms[i] < terms[j]; } );
  uint32_t fp = 1;

  {
    TextWriter w(*seg.postingsWriter.get());
    w.startField(fname);
    for (auto docid: sorted) {
      auto term = terms[docid];
      w.startTerm(term);
      fp += term.size();
      int df = 1;
      fp += df;
      w.startDoc(docid);
      fp += docid;
      for (int j = 0; j < nPosPerDoc; j++) {
        w.addPositionDelta(1);
      }
      w.endDoc(docid);
      w.endTerm(term);
    }
    w.endField();
  }

  seg.initReader();
  PostingsReader& postingsReader = *seg.reader;
  FieldReader& fieldReader = *seg.fieldReader;
  ASSERT_EQ(true, fieldReader.readNextField());
  ASSERT_EQ((std::string_view)fname, seg.fieldReader->name());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, postingsReader, fieldInfo);


  int hitFrac = hitPercent==0 ? -1 : hitPercent * 0x00ffff / 100; // convert to a fraction of 0x00ffff
  uint32_t fingerprint = 1;
  // int32_t hits = 0; int64_t sumdf = 0; int64_t sumdoc = 0;  // some counters to debug where something is off (when matching lucene)
  for (auto _ : state) {
    // hits = 0; sumdf = 0; sumdoc = 0;

    SplitMix64 r2(seed);
    fingerprint = 1;
    for (int i=0; i<nTerms; i++) {
      // go through in unsorted term order
      PackedTerm term;
      bool shouldHit = (int)(r2() & 0x00ffff) <= hitFrac;
      if (shouldHit) {
        term = terms[i];
      } else {
        term = terms[i+nTerms]; // we never indexed second half of array, so it should not be found.
      }
      bool found = tenum.seek((std::string_view)term);
      ASSERT_EQ(found, shouldHit);
      if (found) {
        // hits++;
        fingerprint += tenum.term().size();
        DocsOnlyEnum docsEnum(tenum);
        auto df = docsEnum.numDocs();
        // sumdf += df;
        fingerprint += df;
        for (int j=0; j<df; j++) {
          auto doc = docsEnum.nextDoc();
          // sumdoc += doc;
          fingerprint += doc;
        }
      } else {
        fingerprint *= 3;
      }
    }
    benchmark::DoNotOptimize(fingerprint);
    benchmark::ClobberMemory();
    if (hitPercent >= 100) {
      ASSERT_EQ(fingerprint, fp);
    }
  }

  // std::cout << "fp=" << fp << " fingerprint=" << fingerprint << " hits=" << hits << " sumdf=" << sumdf << " sumdoc=" << sumdoc << std::endl;
  state.counters["fp"] = int32_t(fingerprint) % 100000;  // hmmm, how to get the full resolution on this?  Take mod to try and see least significant digits.
  state.counters["terms"] = nTerms;
  state.counters["isize"] = seg.getIndexSize();
  // state.counters["hits"] = hits; // just a check to see if things are working correctly
}


BENCHMARK_CAPTURE(BM_TermFind, smallHit, 4000000000, 100);
BENCHMARK_CAPTURE(BM_TermFind, smallMiss, 4000000000, 0);
BENCHMARK_CAPTURE(BM_TermFind, small50, 4000000000, 50);
BENCHMARK_CAPTURE(BM_TermFind, medHit, 2000000000000000000, 100);
BENCHMARK_CAPTURE(BM_TermFind, medMiss, 2000000000000000000, 0);
BENCHMARK_CAPTURE(BM_TermFind, med50, 2000000000000000000, 50);
