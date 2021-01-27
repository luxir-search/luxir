#include "solux_bench.h"
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
#include "solux/util/StrRef.h"
#include "solux/util/TermValHash.h"
#include <parallel_hashmap/phmap.h>
#ifdef ROBIN_HOOD_HASHING
#include <robin_hood.h>
#endif

using namespace solux;

struct TestHasher {
  static int call_count;
  using is_transparent = void;

  // The fastest hash by far was XXH3
  size_t h(const char* data, int len) const {
    call_count++;
    // return XXH3_64bits(data, len);
    return XXH3_64bits_withSeed(data, len, 0);
  }

  // also very fast for short keys... this is the one used for all the map comparisons
  // (murmurhash64a) previously, until I realized I was using the wrong XXHash!
  // XXH64 is not XXH3, even if the xxhash release includes it!
  size_t h2(const char* data, int len) const {
    call_count++;
    return Hash::hash(data, len);
  }

  // also very fast for short keys, slower otherwise
  size_t h3(const char* data, int len) const {
    call_count++;
    return Hash::fvn1a(data, len);
  }


  size_t operator()(const char* data, int size) const {
    return h(data, size);
  }

  size_t operator()(const PackedTerm& term) const {
    return h(term.data(), term.size());
  }

  size_t operator()(const std::string& str) const {
    return h(str.data(), str.size());
  }

  size_t operator()(const std::string_view& str) const {
    return h(str.data(), str.size());
  }
};
int TestHasher::call_count = 0;


/** in progress
// Convert phmap's lazy_emplace support for sets into try_emplace that also tells you if the item was inserted!
template <typename SetType, typename KeyType, typename... Args>
inline auto try_emplace(SetType& set, const KeyType& key, Args&&... args) {
  bool inserted = false;
  auto iter = set.lazy_emplace(key, [&](const auto& ctor) {
    ctor(std::forward<Args>(args)...);
    inserted = true;
  });
  return {iter, inserted};
}
**/

// Do a minimal amount of realistic work.
class FakeDocStream {
public:
  int lastDoc;
  int lastPos;
  int doctot;
  int postot;

  FakeDocStream(MemPool& pool, int doc, int pos) {
    unused(pool);
    doctot = 0;
    postot = 0;
    lastDoc = doc;
    lastPos = pos;
  }

  void addDoc(MemPool& pool, int doc, int pos) {
    unused(pool);
    doctot += lastDoc;
    postot += lastPos;
    lastDoc = doc;
    lastPos = pos;
  }
};


class TermValHashShim {
public:
  MemPool& pool;
  TermValHash<FakeDocStream, TestHasher> set;

  TermValHashShim(MemPool& pool, int initialCapacity) : pool(pool), set(pool,initialCapacity) {
  }

  // returns partial fingerprint for comparison across multiple iterations.
  int add(const char* term, int tlen, int doc, int pos) {
    auto [iter, inserted] = set.try_emplace(term, tlen, pool, doc, pos);
    int ret;
    if (!inserted) {
      iter->val().addDoc(pool, doc, pos);
      ret = iter->val().doctot + iter->val().postot;
    } else {
      ret = doc*3 + pos*5;
    }
    return ret;
  }

  int size() {  // number of terms
    return set.size();
  }

  long mem() {  // additional mem usage... framework will take care of pool usage.
    return set.memSize();
  }
};



template <typename phtype> // phmap::flat_hash_set or phmap::node_hash_set type
class PHSet {
public:
  MemPool& pool;
  phtype set;

  PHSet(MemPool& pool, int initialCapacity) : pool(pool), set(initialCapacity) {
  }

  // returns partial fingerprint for comparison across multiple iterations.
  int add(const char* term, int tlen, int doc, int pos) {
    bool inserted = false;
    auto iter = set.lazy_emplace(std::string_view(term, tlen), [&](const auto& ctor) {
      ctor(pool, term, tlen, pool, doc, pos);
      inserted = true;
    });

    // TODO:  try my converter template
    // auto [iter, inserted] = try_emplace(set, std::string_view(term, tlen), pool, term, tlen, doc, pos);
    int ret;
    if (!inserted) {
      iter->val().addDoc(pool, doc, pos);
      ret = iter->val().doctot + iter->val().postot;
    } else {
      ret = doc*3 + pos*5;
    }
    return ret;
  }

  int size() {  // number of terms
    return set.size();
  }

  long mem() {  // additional mem usage... framework will take care of pool usage.
    return set.capacity() * (sizeof(typename phtype::key_type)+1);  // this will only be correct for flat set?
  }
};
using PHFlatSet = PHSet<phmap::flat_hash_set<TermValRef<FakeDocStream>,TestHasher,PackedTermEqual>>;
using PHFlatSetPar = PHSet<phmap::parallel_flat_hash_set<TermValRef<FakeDocStream>,TestHasher,PackedTermEqual>>;
using PHNodeSet = PHSet<phmap::node_hash_set<TermValRef<FakeDocStream>,TestHasher,PackedTermEqual>>;



class PHFlatMap {
public:
  MemPool& pool;
  phmap::flat_hash_map<PackedTerm, FakeDocStream, TestHasher, PackedTermEqual> map;

  PHFlatMap(MemPool& pool, int initialCapacity) : pool(pool), map(initialCapacity) {
  }

  // returns partial fingerprint for comparison across multiple iterations.
  int add(const char* term, int tlen, int doc, int pos) {
    int ret;
    // phmap lazy_emplace expects you to construct a pair, so we can lazily create the key as well!

    bool inserted = false;
    auto iter = map.lazy_emplace(std::string_view(term,tlen), [&](const auto& ctor) {
      ctor(std::pair(PackedTerm(pool,term,tlen), FakeDocStream(pool, doc, pos)));  // will this construct and avoid making a copy of the value?
      inserted = true;
    });

    if (!inserted) {
      auto&[k,v] = *iter;
      v.addDoc(pool, doc, pos);
      ret = v.doctot + v.postot;
    } else {
      ret = doc*3 + pos*5;
    }

    return ret;
  }

  int size() {  // number of terms
    return map.size();
  }

  long mem() {  // additional mem usage... framework will take care of pool usage.
    return map.capacity() * sizeof(decltype(map)::value_type);
  }
};

template <typename MapType>
class SimpleMap {
public:
  MemPool& pool;
  MapType map;
  using key_type = typename MapType::key_type;

  SimpleMap(MemPool& pool, int initialCapacity) : pool(pool), map(initialCapacity) {
  }

  // returns partial fingerprint for comparison across multiple iterations.
  int add(const char* term, int tlen, int doc, int pos) {
    // auto sv = std::string_view(term,tlen);
    auto sv = key_type(term, tlen); // hamstring phmap for now and don't use heterogeneous lookup (since robinmap can't do it yet, and we want to compare)
    auto [iter, inserted] = map.try_emplace(sv, pool, doc, pos);
    int ret;
    if (!inserted) {
      auto&[k,v] = *iter;
      v.addDoc(pool, doc, pos);
      ret = v.doctot + v.postot;
    } else {
      ret = doc*3 + pos*5;
    }

    return ret;
  }

  int size() {  // number of terms
    return map.size();
  }

  long mem() {  // additional mem usage... framework will take care of pool usage.
    return 0;
  }
};

// Look up or create a term and add a document to it
// using InvertImpl = OldTermValHash;
template <typename InvertImpl>
static void BM_invertTemplate(benchmark::State& state) {
  Rng r(1);
  MemPool pool;

  // A small random pool to make terms from. This won't realistically test
  // hash quality, but will be better to try and isolate performance of the map implementation otherwise.
  // We only need about 4K when testing up to 16 byte keys since we will get collisions due to the small key
  // space anyway.  Reduce the size to get more collisions.
  static constexpr int RAND_POOL_SIZE = 16384;  // make power of two so we can efficiently mask.

  // block of random data
  char* data = pool.allocate(RAND_POOL_SIZE);
  auto data64 = (uint64_t*)data;
  for (int i=0; i<(int)(RAND_POOL_SIZE/sizeof(uint64_t)); i++) {
    data64[i] = r();
  }

  MemPool::save_point savePoint = pool.getSavePoint();

  int nTerms = 1000000;  // number of lookups to do
  if (solux::unit_tests) {
    nTerms = 1000;
  }
  int unique = 0;        // number of terms that turned out to be unique
  long poolsz = 0;
  long mem = 0;
  int result = 0;

  for (auto _ : state) {
    TestHasher::call_count = 0;

    result = 0;

    r.init(2);
    pool.rewind(savePoint, 100);  // save a lot of buffers to better isolate map costs (only needed if actually adding more data!)
    long startsize = pool.size();

    InvertImpl impl(pool, 8);

    int randOffset = 0;
    for (int i=0; i<nTerms; i++) {
      char* term;
      int tlen;
      for(;;) {
        term = data + randOffset;

        // comparing solux impl of TermValHash with phmap::unordered_flat_set
        // Note: most of these numbers were with murmurhash, but now we switched to XXH3!
        // tlen = (r()&0x3f) + 4;   // 655K unique keys (long): phmap 6.5% better (prob because skipping long key comps)
        // tlen = (r()&0x0f) + 4;   // 256k unique keys (shortish): TVHash better by 1.5%
        tlen = (r()&0x07) + 8;   // 130k unique keys (med): tie
        // tlen = (r()%10 + 1);     // 145K unique short keys: TVHash better by 7.8%
        // tlen = (r()%9 + 1);      // 129K unique short keys: TVHash better by 15.3%
        // tlen = (r()%8 + 1);      // 113K unique short keys: TVHash better by 9.2% (TVHash rehashed sooner)
        // tlen = (r()%7 + 1);      // 96K unique short keys: tie (phmap rehashed sooner, may not fit in cache as well?)
        // tlen = (r()%6 + 1);      // 80K unique short keys: TVHash better by 12.8%
        // tlen = (r()%5 + 1);      // 64K unique short keys: phmap better by 12%
        // tlen = (r()%4 + 1);      // 47K unique short keys: phmap better by 13%
        // tlen = (r()%2 + 3);      // 32K unique short keys: phmap better by 16.5%
        // tlen = (r()%2 + 2);      // 30K unique short keys: phmap better by 6%
        // tlen = 2;                // 7.7K unique short keys: results all over the map... 67M/sec to 135M/sec
                                    //    no matter what min-time the benchmark runs for!  clock freq scaling can't
                                    //    account for this! What core the proc is scheduled on in relation to mem?
                                    //    We are probably memory-bound somewhere and seeing the mem arch.

        randOffset += tlen;
        if (randOffset >= RAND_POOL_SIZE) {
          randOffset = 0;
          continue;
        }
        break;
      }

      uint64_t x = r();
      int doc = (int)x;
      int pos = x>>32;

      result += impl.add(term, tlen, doc, pos);
    }

    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();

    // std::cout << " result=" << result << " nTerms=" << nTerms << " setSize=" << impl.size() << std::endl;

    poolsz = pool.size() - startsize;
    unique = impl.size();
    mem = poolsz + impl.mem();
  }

  state.counters["fp"] = result % 100000;  // fingerprint to make sure we are doing the same thing with different implementations
  state.counters["inserts"] = nTerms;
  state.counters["rate"] = benchmark::Counter(nTerms, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["unique"] = unique;
  state.counters["mem"] = (double)mem;  // TODO: not complete... measure via generic API call?
  state.counters["hashes"] = TestHasher::call_count;
}

// TODO: implement try_emplace for robin_hood
// TODO: why is phmap flat_hash_set slower?  One trick I'm using is "zero constructible" (overlaying over zero memory)
// TODO: try some map implementations... but can one get try_emplace to work with lazy keys as well a values?
// TODO: make my TermValHash more standards compliant
// TODO: try different sizes... could just be a sizing / resizing issue?  For that, need key sizing.


using StrPHFlatMap = SimpleMap<phmap::flat_hash_map<std::string, FakeDocStream, TestHasher, PackedTermEqual>>;
using SVPHFlatMap = SimpleMap<phmap::flat_hash_map<std::string_view, FakeDocStream, TestHasher, PackedTermEqual>>;
using SVstdMap = SimpleMap<std::unordered_map<std::string_view, FakeDocStream, TestHasher, PackedTermEqual>>;

// BENCHMARK(BM_invertTemplate<OldTermValHash>); // doesn't work, so we'll use this longer form
static void BM_invertTermValHash(benchmark::State& state) { BM_invertTemplate<TermValHashShim>(state); }
BENCHMARK(BM_invertTermValHash);
static void BM_invertPHFlatSet(benchmark::State& state) { BM_invertTemplate<PHFlatSet>(state); }
BENCHMARK(BM_invertPHFlatSet);
static void BM_invertPHFlatSetPar(benchmark::State& state) { BM_invertTemplate<PHFlatSetPar>(state); }
BENCHMARK(BM_invertPHFlatSetPar);
static void BM_invertPHNodeSet(benchmark::State& state) { BM_invertTemplate<PHNodeSet>(state); }
BENCHMARK(BM_invertPHNodeSet);
static void BM_invertPHFlatMap(benchmark::State& state) { BM_invertTemplate<PHFlatMap>(state); }
BENCHMARK(BM_invertPHFlatMap);
static void BM_invertStrPHFlatMap(benchmark::State& state) { BM_invertTemplate<StrPHFlatMap>(state); }
BENCHMARK(BM_invertStrPHFlatMap);
static void BM_invertSVPHFlatMap(benchmark::State& state) { BM_invertTemplate<SVPHFlatMap>(state); }
BENCHMARK(BM_invertSVPHFlatMap);
static void BM_invertSVstdMap(benchmark::State& state) { BM_invertTemplate<SVstdMap>(state); }
BENCHMARK(BM_invertSVstdMap);

#ifdef ROBIN_HOOD_HASHING
using StrRobinFlatMap = SimpleMap<robin_hood::unordered_flat_map<std::string, FakeDocStream, TestHasher, PackedTermEqual>>;
using SVRobinFlatMap = SimpleMap<robin_hood::unordered_flat_map<std::string_view, FakeDocStream, TestHasher, PackedTermEqual>>;
static void BM_invertStrRobinFlatMap(benchmark::State& state) { BM_invertTemplate<StrRobinFlatMap>(state); }
BENCHMARK(BM_invertStrRobinFlatMap);
static void BM_invertSVRobinFlatMap(benchmark::State& state) { BM_invertTemplate<SVRobinFlatMap>(state); }
BENCHMARK(BM_invertSVRobinFlatMap);
#endif

/*
Due to robin_hood's incomplete heterogeneous lookup support (as of 1/20201) as well as generally slower speed than phmap,
the latter is being selected as the general hash table impl for solux.  To avoid another dependency, the robin_hood code
is ifdefed out.

Representative run:
 [main] /mnt/e/opt/code/solux/cmake-build-release-wsl/bin$ ./solux_test --bench --benchmark_filter=BM_invert --benchmark_repetitions=60 | grep _mean
2021-01-26T14:47:13-05:00
Running ./solux_test
        Run on (12 X 3593.26 MHz CPU s)
CPU Caches:
L1 Data 32 KiB (x6)
L1 Instruction 32 KiB (x6)
L2 Unified 512 KiB (x6)
L3 Unified 16384 KiB (x1)
Load Average: 0.23, 0.28, 0.21
BM_invertTermValHash_mean         41199097 ns     41196780 ns           60 fp=11.663k hashes=1.19584M inserts=1000k mem=5.98245M rate=24.31M/s unique=145.498k
BM_invertPHFlatSet_mean           43621475 ns     43621014 ns           60 fp=11.663k hashes=1.22758M inserts=1000k mem=6.24456M rate=22.9891M/s unique=145.498k
BM_invertPHNodeSet_mean           62734916 ns     62733826 ns           60 fp=11.663k hashes=1.22758M inserts=1000k mem=6.24456M rate=15.9481M/s unique=145.498k
BM_invertPHFlatMap_mean           41037557 ns     41037360 ns           60 fp=11.663k hashes=1.22758M inserts=1000k mem=7.31612M rate=24.3691M/s unique=145.498k
BM_invertStrPHFlatMap_mean        40987177 ns     40987275 ns           60 fp=11.663k hashes=1.22758M inserts=1000k mem=0 rate=24.4009M/s unique=145.498k
BM_invertStrRobinFlatMap_mean     51969287 ns     51969400 ns           60 fp=11.663k hashes=1.35523M inserts=1000k mem=0 rate=19.2539M/s unique=145.498k
BM_invertSVPHFlatMap_mean         29640047 ns     29640118 ns           60 fp=11.663k hashes=1.22758M inserts=1000k mem=0 rate=33.7395M/s unique=145.498k
BM_invertSVRobinFlatMap_mean      39307582 ns     39307677 ns           60 fp=11.663k hashes=1.35523M inserts=1000k mem=0 rate=25.4411M/s unique=145.498k
BM_invertSVstdMap_mean            64080090 ns     64080268 ns           60 fp=11.663k hashes=1.1455M inserts=1000k mem=0 rate=15.6162M/s unique=145.498k
*/