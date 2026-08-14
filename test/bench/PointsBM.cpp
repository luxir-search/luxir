#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/index/PointsWriter.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/store/Directory.h"
#include "luxir/util/random.h"
#include "luxir/util/screaming.h"

using namespace luxir;

namespace {

enum PointShape : uint8_t {
  UNIQUE_CORRELATED,
  UNIQUE_SHUFFLED,
  DUP64_SHUFFLED,
  GAPPY_ASCENDING,
  GCD1000_SHUFFLED
};

enum FenceArm : uint8_t { LEAF_MAX, MIN_ONLY };

struct ShapeSpec {
  PointShape shape;
  std::string_view name;
};

struct FixtureKey {
  PointShape shape;
  uint16_t leafSize;
  bool bitsetDocids;
  bool valueGcd;

  bool operator<(const FixtureKey &other) const {
    return std::tie(shape, leafSize, bitsetDocids, valueGcd) <
           std::tie(other.shape, other.leafSize, other.bitsetDocids,
                    other.valueGcd);
  }
};

struct BoundsCase {
  int64_t lo;
  int64_t hi;
  uint64_t begin;
  uint64_t end;
};

constexpr std::array<ShapeSpec, 5> SHAPES{{
    {UNIQUE_CORRELATED, "unique_correlated"},
    {UNIQUE_SHUFFLED, "unique_shuffled"},
    {DUP64_SHUFFLED, "dup64_shuffled"},
    {GAPPY_ASCENDING, "gappy_ascending"},
    {GCD1000_SHUFFLED, "gcd1000_shuffled"},
}};

constexpr std::array<uint16_t, 4> LEAF_SIZES{{128, 256, 512, 1024}};
constexpr std::array<int32_t, 4> WINDOW_PERMILLE{{10, 100, 500, 900}};

uint64_t pointCount() {
  return luxir::unit_tests
      ? (uint64_t)LuxirTest::scaleTestWork(8 * 1024)
      : 1'000'000ULL;
}

int64_t valueAtOrdinal(PointShape shape, uint64_t ordinal) {
  switch (shape) {
  case UNIQUE_CORRELATED:
  case UNIQUE_SHUFFLED:
    return (int64_t)ordinal * 2 + 7;
  case DUP64_SHUFFLED:
    return (int64_t)(ordinal / 64);
  case GAPPY_ASCENDING:
    return (int64_t)ordinal * 3;
  case GCD1000_SHUFFLED:
    return (int64_t)(ordinal / 4) * 1000;
  }
  return 0;
}

uint64_t scalarBound(PointShape shape, uint64_t count, int64_t target,
                     bool upper) {
  uint64_t lo = 0;
  uint64_t hi = count;
  while (lo < hi) {
    uint64_t mid = lo + (hi - lo) / 2;
    int64_t value = valueAtOrdinal(shape, mid);
    if (value < target || (upper && value == target))
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

BoundsCase centeredBounds(PointShape shape, uint64_t count, uint16_t leafSize,
                          bool alignFirst) {
  uint64_t targetBegin = count / 4 + 97;
  uint64_t targetEnd = count * 3 / 4 - 83;
  if (alignFirst) {
    targetBegin = ((count / 4 + leafSize - 1) / leafSize) * leafSize;
    targetEnd = targetBegin + count / 2 - 83;
  }
  int64_t lo = valueAtOrdinal(shape, targetBegin);
  int64_t hi = valueAtOrdinal(shape, targetEnd - 1);
  return {lo, hi, scalarBound(shape, count, lo, false),
          scalarBound(shape, count, hi, true)};
}

std::vector<int32_t> shuffledDocs(uint64_t count, PointShape shape) {
  std::vector<int32_t> docs((size_t)count);
  std::iota(docs.begin(), docs.end(), 0);
  SplitMix64 rng(0x5f6d4c3b2a190817ULL + (uint64_t)shape);
  for (uint64_t i = count - 1; i > 0; i--) {
    std::swap(docs[(size_t)i], docs[(size_t)rng.rint((int64_t)i + 1)]);
  }
  return docs;
}

class PointsFixture {
  RAMDir dir;
  std::shared_ptr<InputFile> inputFile;
  InputStream input;
  std::unique_ptr<PointsReader> points;
  PointShape shape;
  uint64_t count;
  int32_t maxDoc = 0;

  void writePoints(PointsWriter &writer) {
    if (shape == UNIQUE_CORRELATED) {
      for (uint64_t ordinal = 0; ordinal < count; ordinal++) {
        writer.addPoint(valueAtOrdinal(shape, ordinal), (int32_t)ordinal);
      }
      maxDoc = (int32_t)count;
      return;
    }

    if (shape == GAPPY_ASCENDING) {
      SplitMix64 rng(0xa17c9e5b3d2468f0ULL);
      int32_t docid = -1;
      for (uint64_t ordinal = 0; ordinal < count; ordinal++) {
        do {
          docid++;
        } while ((rng() & 1) == 0);
        writer.addPoint(valueAtOrdinal(shape, ordinal), docid);
      }
      maxDoc = docid + 1;
      return;
    }

    std::vector<int32_t> docs = shuffledDocs(count, shape);
    uint32_t groupSize = shape == DUP64_SHUFFLED ? 64 : 1;
    if (shape == GCD1000_SHUFFLED)
      groupSize = 4;
    if (groupSize > 1) {
      for (size_t begin = 0; begin < docs.size(); begin += groupSize) {
        size_t end = std::min(docs.size(), begin + groupSize);
        std::sort(docs.begin() + (int64_t)begin, docs.begin() + (int64_t)end);
      }
    }
    for (uint64_t ordinal = 0; ordinal < count; ordinal++) {
      writer.addPoint(valueAtOrdinal(shape, ordinal), docs[(size_t)ordinal]);
    }
    maxDoc = (int32_t)count;
  }

public:
  PointsFixture(PointShape shape, PointsWriter::Options options)
      : shape(shape), count(pointCount()) {
    auto file = dir.createFile("points");
    OutputStream out(file.get());
    out.streamNumber = 0;
    out.writeBytes("pre");
    PointsWriter writer(out, options);
    writePoints(writer);
    PointsWriter::Data data = writer.finish();
    out.close();
    dir.finishFile(*file);

    inputFile = dir.openFile("points");
    input = inputFile->getInputStream();
    points = std::make_unique<PointsReader>(input, data.pointsLoc.offset(),
                                            data.pointsMetaOff);
    points->validate();
    if (points->pointCount() != count) {
      throw std::logic_error("points benchmark point count mismatch");
    }
    if (shape == GAPPY_ASCENDING) {
      uint8_t expected = options.bitsetDocids ? PointsWriter::DOC_BITSET
                                              : PointsWriter::DOC_FOR;
      for (uint32_t leaf = 0; leaf < points->leafCount(); leaf++) {
        if (points->leafInfo(leaf).docCodec != expected) {
          throw std::logic_error("points benchmark docid codec mismatch");
        }
      }
    }
  }

  PointsReader &reader() { return *points; }
  PointShape pointShape() const { return shape; }
  uint64_t size() const { return count; }
  int32_t maxDocCount() const { return maxDoc; }
};

PointsFixture &fixture(PointShape shape, PointsWriter::Options options) {
  static std::map<FixtureKey, std::unique_ptr<PointsFixture>> fixtures;
  FixtureKey key{shape, options.maxPointsPerLeaf, options.bitsetDocids,
                 options.valueGcd};
  auto found = fixtures.find(key);
  if (found == fixtures.end()) {
    found =
        fixtures.emplace(key, std::make_unique<PointsFixture>(shape, options))
            .first;
  }
  return *found->second;
}

bool skipReducedUnitCase(benchmark::State &state, uint16_t leafSize,
                         int32_t windowPermille = -1) {
  if (!luxir::unit_tests)
    return false;
  if (leafSize != 256 && leafSize != 512) {
    state.SkipWithMessage("reduced unit-test leaf-size sweep");
    return true;
  }
  if (windowPermille >= 0 && windowPermille != 100 && windowPermille != 900) {
    state.SkipWithMessage("reduced unit-test materialize windows");
    return true;
  }
  return false;
}

void setRun(screaming::FixedBitSet &bits, int32_t begin, int32_t end) {
  if (begin >= end)
    return;
  int32_t firstWord = begin >> 6;
  int32_t lastWord = (end - 1) >> 6;
  uint64_t firstMask = ~0ULL << (begin & 63);
  uint64_t lastMask = (end & 63) == 0 ? ~0ULL : (1ULL << (end & 63)) - 1ULL;
  if (firstWord == lastWord) {
    bits.words[firstWord] |= firstMask & lastMask;
    return;
  }
  bits.words[firstWord] |= firstMask;
  for (int32_t word = firstWord + 1; word < lastWord; word++) {
    bits.words[word] = ~0ULL;
  }
  bits.words[lastWord] |= lastMask;
}

std::pair<uint64_t, uint64_t>
exactPositions(PointsReader &points, const PointsReader::FenceRange &fence,
               int64_t lo, int64_t hi, std::span<uint32_t> residualScratch,
               std::span<int64_t> rawScratch) {
  if (fence.empty)
    return {0, 0};
  auto first =
      points.valueBounds(fence.firstLeaf, lo, hi, residualScratch, rawScratch);
  uint64_t loPos = points.leafOrdinalStart(fence.firstLeaf) + first.lower;
  // The min-only arm can fence one disjoint leaf early. Its end is the
  // insertion position for the following leaf.
  if (first.lower == 0 && first.upper == 0 &&
      fence.firstLeaf < fence.lastLeaf) {
    loPos = points.leafOrdinalEnd(fence.firstLeaf);
  }
  if (fence.firstLeaf == fence.lastLeaf) {
    return {loPos, points.leafOrdinalStart(fence.firstLeaf) + first.upper};
  }
  auto last =
      points.valueBounds(fence.lastLeaf, lo, hi, residualScratch, rawScratch);
  return {loPos, points.leafOrdinalStart(fence.lastLeaf) + last.upper};
}

uint32_t firstLeafEarlyCount(PointsReader &points) {
  if (points.leafCount() < 2)
    return 0;
  uint32_t probes = std::min<uint32_t>(64, points.leafCount() - 1);
  uint32_t early = 0;
  for (uint32_t probe = 0; probe < probes; probe++) {
    uint32_t leaf =
        1 + (uint32_t)((uint64_t)probe * (points.leafCount() - 1) / probes);
    int64_t value = points.leafMin(leaf);
    auto leafMax = points.fenceRange(value, value);
    auto minOnly = points.fenceRangeMinOnlyForBench(value, value);
    if (!leafMax.empty && !minOnly.empty &&
        minOnly.firstLeaf < leafMax.firstLeaf) {
      early++;
    }
  }
  return early;
}

void BM_PointsMaterialize(benchmark::State &state, PointShape shape,
                          PointsWriter::Options options) {
  int32_t windowPermille = (int32_t)state.range(0);
  if (skipReducedUnitCase(state, options.maxPointsPerLeaf, windowPermille))
    return;

  PointsFixture &data = fixture(shape, options);
  PointsReader &points = data.reader();
  uint64_t window = data.size() * (uint64_t)windowPermille / 1000;
  uint64_t begin = (data.size() - window) / 2;
  uint64_t end = begin + window;
  std::vector<uint32_t> docScratch(points.maxPointsPerLeaf());
  std::vector<uint64_t> words(
      screaming::FixedBitSet::sizeInWords(data.maxDocCount()));
  screaming::FixedBitSet bits(words.data(), data.maxDocCount());

  for (auto _ : state) {
    std::fill(words.begin(), words.end(), 0);
    uint64_t emitted = 0;
    points.emitOrdinalRange(
        begin, end, docScratch,
        [&bits, &emitted](int32_t doc) {
          bits.set(doc);
          emitted++;
        },
        [&bits, &emitted](int32_t runBegin, int32_t runEnd) {
          setRun(bits, runBegin, runEnd);
          emitted += (uint64_t)(runEnd - runBegin);
        },
        [&bits, &emitted](int32_t wordIndex, uint64_t word) {
          bits.words[wordIndex] |= word;
          emitted += (uint64_t)std::popcount(word);
        });
    benchmark::ClobberMemory();
    benchmark::DoNotOptimize(bits.words);
    benchmark::DoNotOptimize(emitted);
    if (emitted != window) {
      state.SkipWithError("materialized point count mismatch");
      break;
    }
  }
  state.counters["max_doc"] = data.maxDocCount();
  state.counters["points"] = (double)data.size();
  state.counters["window_points"] = (double)window;
}

void BM_PointsBounds(benchmark::State &state, PointShape shape,
                     PointsWriter::Options options, FenceArm arm,
                     bool reportEarly) {
  if (skipReducedUnitCase(state, options.maxPointsPerLeaf))
    return;

  PointsFixture &data = fixture(shape, options);
  PointsReader &points = data.reader();
  BoundsCase bounds = centeredBounds(data.pointShape(), data.size(),
                                     options.maxPointsPerLeaf, reportEarly);
  std::vector<uint32_t> residualScratch(points.maxPointsPerLeaf());
  std::vector<int64_t> rawScratch(points.maxPointsPerLeaf());
  uint32_t early = reportEarly ? firstLeafEarlyCount(points) : 0;

  for (auto _ : state) {
    PointsReader::FenceRange fence =
        arm == LEAF_MAX
            ? points.fenceRange(bounds.lo, bounds.hi)
            : points.fenceRangeMinOnlyForBench(bounds.lo, bounds.hi);
    auto positions = exactPositions(points, fence, bounds.lo, bounds.hi,
                                    residualScratch, rawScratch);
    benchmark::DoNotOptimize(positions);
    if (positions.first != bounds.begin || positions.second != bounds.end) {
      state.SkipWithError("point bounds differ from scalar reference");
      break;
    }
  }
  state.counters["matches"] = (double)(bounds.end - bounds.begin);
  state.counters["points"] = (double)data.size();
  if (reportEarly)
    state.counters["first_leaf_early"] = early;
}

void BM_PointsSize(benchmark::State &state, PointShape shape,
                   PointsWriter::Options options) {
  if (skipReducedUnitCase(state, options.maxPointsPerLeaf))
    return;

  PointsFixture &data = fixture(shape, options);
  for (auto _ : state) {
    benchmark::DoNotOptimize(data.reader());
  }
  // Counters read outside the timed loop: a cross-loop scalar carried
  // through DoNotOptimize misreads under gcc-release, the same harness
  // fragility the phrase bench hit. The reads are what this bench reports;
  // the loop exists only to satisfy the benchmark runner.
  uint64_t bytes = data.reader().sizeInBytes();
  state.counters["bytes_per_point"] = (double)bytes / (double)data.size();
  state.counters["leaf_count"] = data.reader().leafCount();
  state.counters["size_bytes"] = (double)bytes;
}

std::string benchName(const ShapeSpec &shape, std::string_view op,
                      uint16_t leafSize, std::string_view suffix) {
  std::string name = "BM_Points/" + std::string(shape.name) + "/" +
                     std::string(op) + "/leaf:" + std::to_string(leafSize);
  if (!suffix.empty())
    name += "/" + std::string(suffix);
  return name;
}

void registerMaterialize(const ShapeSpec &shape, PointsWriter::Options options,
                         std::string_view suffix = {}) {
  auto *registration = benchmark::RegisterBenchmark(
      benchName(shape, "materialize", options.maxPointsPerLeaf, suffix),
      [pointShape = shape.shape, options](benchmark::State &state) {
        BM_PointsMaterialize(state, pointShape, options);
      });
  registration->ArgName("window_permille");
  for (int32_t perMille : WINDOW_PERMILLE)
    registration->Arg(perMille);
  registration->UseRealTime();
}

void registerBounds(const ShapeSpec &shape, PointsWriter::Options options,
                    FenceArm arm = LEAF_MAX, std::string_view suffix = {},
                    bool reportEarly = false) {
  benchmark::RegisterBenchmark(
      benchName(shape, "bounds", options.maxPointsPerLeaf, suffix),
      [pointShape = shape.shape, options, arm,
       reportEarly](benchmark::State &state) {
        BM_PointsBounds(state, pointShape, options, arm, reportEarly);
      })
      ->UseRealTime();
}

void registerSize(const ShapeSpec &shape, PointsWriter::Options options,
                  std::string_view suffix = {}) {
  benchmark::RegisterBenchmark(
      benchName(shape, "size", options.maxPointsPerLeaf, suffix),
      [pointShape = shape.shape, options](benchmark::State &state) {
        BM_PointsSize(state, pointShape, options);
      })
      ->Iterations(1);
}

[[maybe_unused]] bool benchmarksRegistered = [] {
  for (const ShapeSpec &shape : SHAPES) {
    for (uint16_t leafSize : LEAF_SIZES) {
      PointsWriter::Options options{.maxPointsPerLeaf = leafSize};
      registerMaterialize(shape, options);
      registerBounds(shape, options);
      registerSize(shape, options);
    }
  }

  const ShapeSpec &gappy = SHAPES[GAPPY_ASCENDING];
  for (bool bitset : {true, false}) {
    PointsWriter::Options options{
        .maxPointsPerLeaf = 512,
        .bitsetDocids = bitset,
    };
    std::string_view suffix = bitset ? "docids:bitset" : "docids:for";
    registerMaterialize(gappy, options, suffix);
    registerSize(gappy, options, suffix);
  }

  for (PointShape pointShape : {GCD1000_SHUFFLED, UNIQUE_CORRELATED}) {
    const ShapeSpec &shape = SHAPES[(size_t)pointShape];
    for (bool gcd : {true, false}) {
      PointsWriter::Options options{
          .maxPointsPerLeaf = 512,
          .valueGcd = gcd,
      };
      std::string_view suffix = gcd ? "values:gcd" : "values:min_for";
      registerBounds(shape, options, LEAF_MAX, suffix);
      registerSize(shape, options, suffix);
    }
  }

  for (const ShapeSpec &shape : SHAPES) {
    PointsWriter::Options options{.maxPointsPerLeaf = 512};
    registerBounds(shape, options, LEAF_MAX, "fence:leaf_max", true);
    registerBounds(shape, options, MIN_ONLY, "fence:min_only", true);
  }
  return true;
}();

} // namespace
