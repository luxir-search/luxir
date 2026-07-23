#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "solux/codec/LinearPack.h"

#if !defined(__AVX2__)
#error "solux_ord_bulk_bench requires AVX2"
#endif

namespace solux {
namespace {

constexpr uint64_t DEFAULT_VALUE_COUNT = 1ULL << 22;
constexpr double DEFAULT_MIN_MS = 8.0;
constexpr int32_t SAMPLES = 3;

struct Options {
  uint64_t valueCount = DEFAULT_VALUE_COUNT;
  double minMs = DEFAULT_MIN_MS;
  bool quick = false;
};

struct Measurement {
  double nsPerTouch = 0.0;
  uint64_t checksum = 0;
  uint64_t repeats = 0;
};

struct BulkResult {
  uint32_t size = 0;
  Measurement timing;
};

uint64_t mix64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int32_t i = 1; i < argc; i++) {
    std::string_view arg(argv[i]);
    if (arg == "--quick") {
      options.quick = true;
    } else if (arg.starts_with("--values=")) {
      options.valueCount = std::strtoull(argv[i] + 9, nullptr, 10);
    } else if (arg.starts_with("--min-ms=")) {
      options.minMs = std::strtod(argv[i] + 9, nullptr);
    } else if (arg == "--help") {
      std::cout
          << "usage: solux_ord_bulk_bench [--quick] [--values=N] [--min-ms=N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(arg));
    }
  }
  if (options.quick) {
    if (options.valueCount == DEFAULT_VALUE_COUNT) {
      options.valueCount = 1ULL << 18;
    }
    if (options.minMs == DEFAULT_MIN_MS) {
      options.minMs = 0.5;
    }
  }
  if (options.valueCount < 4096 || options.minMs <= 0.0) {
    throw std::invalid_argument("values must be >= 4096 and min-ms must be > 0");
  }
  return options;
}

std::vector<char> makeColumn(uint64_t valueCount, uint8_t bits) {
  std::vector<char> encoded;
  encoded.reserve((size_t)LinearPack::byteSize(valueCount, bits));
  LinearPack::Writer writer(encoded, bits);
  const uint32_t mask = LinearPack::mask32(bits);
  for (uint64_t i = 0; i < valueCount; i++) {
    writer.append((uint32_t)mix64(i) & mask);
  }
  writer.finish();
  return encoded;
}

std::vector<uint32_t> makeUniformTouches(uint64_t valueCount, double density) {
  std::vector<uint32_t> touches;
  uint64_t expected = (uint64_t)std::ceil((double)valueCount * density);
  touches.reserve((size_t)std::min(valueCount, expected + expected / 50 + 16));
  if (density >= 1.0) {
    for (uint64_t i = 0; i < valueCount; i++) {
      touches.push_back((uint32_t)i);
    }
    return touches;
  }

  const uint64_t threshold =
      (uint64_t)(density * (double)std::numeric_limits<uint64_t>::max());
  for (uint64_t i = 0; i < valueCount; i++) {
    if (mix64(i ^ 0x6a09e667f3bcc909ULL) <= threshold) {
      touches.push_back((uint32_t)i);
    }
  }
  if (touches.empty()) {
    touches.push_back((uint32_t)(mix64(valueCount) % valueCount));
  }
  return touches;
}

std::vector<uint32_t> makeRunTouches(uint64_t valueCount, double density) {
  constexpr uint32_t RUNS = 4;
  uint64_t total = (uint64_t)std::llround((double)valueCount * density);
  total = std::clamp(total, (uint64_t)1, valueCount);
  std::vector<uint32_t> touches;
  touches.reserve((size_t)total);
  const uint64_t perRun = total / RUNS;
  const uint64_t extra = total % RUNS;
  for (uint32_t run = 0; run < RUNS; run++) {
    const uint64_t segmentStart = valueCount * run / RUNS;
    const uint64_t segmentEnd = valueCount * (run + 1) / RUNS;
    const uint64_t count = perRun + (run < extra ? 1 : 0);
    const uint64_t start = segmentStart + (segmentEnd - segmentStart - count) / 2;
    for (uint64_t i = 0; i < count; i++) {
      touches.push_back((uint32_t)(start + i));
    }
  }
  return touches;
}

[[gnu::noinline]] uint64_t servicePoint(
    const char* encoded, uint8_t bits, uint32_t mask,
    const std::vector<uint32_t>& touches) {
  uint64_t checksum = 0;
  for (uint32_t index : touches) {
    checksum += LinearPack::select32(encoded, index, bits, mask);
  }
  return checksum;
}

template<uint32_t N>
class ChunkCache {
  const char* encoded;
  uint64_t valueCount;
  int64_t decodedStart = -1;
  int64_t decodedEnd = -1;
  uint8_t bits;
  uint32_t mask;
  alignas(64) std::array<uint32_t, N> decoded;

  void decode(uint32_t index) {
    decodedStart = (index / N) * N;
    decodedEnd = (int64_t)std::min(
        (uint64_t)decodedStart + N, valueCount);
    const uint32_t count = (uint32_t)(decodedEnd - decodedStart);
    LinearPack::unpackBlock<N>(
        encoded, (uint64_t)decodedStart, count, bits, mask, decoded.data());
  }

public:
  ChunkCache(const char* encoded, uint64_t valueCount,
             uint8_t bits, uint32_t mask)
      : encoded(encoded), valueCount(valueCount), bits(bits), mask(mask) {}

  uint32_t valueAt(uint32_t index) {
    if (index < decodedStart || index >= decodedEnd) {
      decode(index);
    }
    return decoded[(size_t)(index - decodedStart)];
  }
};

template<uint32_t N>
[[gnu::noinline]] uint64_t serviceBulk(
    const char* encoded, uint64_t valueCount, uint8_t bits, uint32_t mask,
    const std::vector<uint32_t>& touches) {
  ChunkCache<N> cache(encoded, valueCount, bits, mask);
  uint64_t checksum = 0;
  for (uint32_t index : touches) {
    checksum += cache.valueAt(index);
  }
  return checksum;
}

template<typename Service>
Measurement measure(Service&& service, uint64_t touchCount, double minMs) {
  using Clock = std::chrono::steady_clock;
  const uint64_t expected = service();

  auto singleStart = Clock::now();
  asm volatile("" ::: "memory");
  const uint64_t singleChecksum = service();
  asm volatile("" ::: "memory");
  auto singleEnd = Clock::now();
  if (singleChecksum != expected) {
    throw std::runtime_error("unstable benchmark checksum");
  }
  double singleNs =
      std::chrono::duration<double, std::nano>(singleEnd - singleStart).count();
  singleNs = std::max(singleNs, 1.0);
  uint64_t repeats = (uint64_t)std::ceil(minMs * 1'000'000.0 / singleNs);
  repeats = std::clamp(
      repeats, (uint64_t)1, (uint64_t)10'000'000);

  double best = std::numeric_limits<double>::infinity();
  uint64_t measuredChecksum = 0;
  for (int32_t sample = 0; sample < SAMPLES; sample++) {
    uint64_t accumulator = 0;
    auto start = Clock::now();
    for (uint64_t repeat = 0; repeat < repeats; repeat++) {
      asm volatile("" ::: "memory");
      uint64_t checksum = service();
      asm volatile("" ::: "memory");
      if (checksum != expected) {
        throw std::runtime_error("benchmark checksum mismatch");
      }
      accumulator = accumulator * 0xd6e8feb86659fd93ULL + checksum + repeat;
    }
    auto end = Clock::now();
    const double elapsed =
        std::chrono::duration<double, std::nano>(end - start).count();
    best = std::min(best, elapsed / ((double)touchCount * (double)repeats));
    measuredChecksum ^= accumulator;
  }
  asm volatile("" : "+r"(measuredChecksum) :: "memory");
  return {best, expected, repeats};
}

template<uint32_t N>
BulkResult measureBulk(const std::vector<char>& encoded, uint64_t valueCount,
                       uint8_t bits, uint32_t mask,
                       const std::vector<uint32_t>& touches, double minMs,
                       uint64_t expectedChecksum) {
  Measurement timing = measure(
      [&]() {
        return serviceBulk<N>(
            encoded.data(), valueCount, bits, mask, touches);
      },
      touches.size(), minMs);
  if (timing.checksum != expectedChecksum) {
    throw std::runtime_error("bulk decode disagrees with point access");
  }
  return {N, timing};
}

std::vector<BulkResult> measureBulkSizes(
    const std::vector<char>& encoded, uint64_t valueCount,
    uint8_t bits, uint32_t mask, const std::vector<uint32_t>& touches,
    double minMs, uint64_t expectedChecksum) {
  std::vector<BulkResult> results;
  results.reserve(10);
  results.push_back(measureBulk<8>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<16>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<32>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<64>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<128>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<256>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<512>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<1024>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<2048>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  results.push_back(measureBulk<4096>(
      encoded, valueCount, bits, mask, touches, minMs, expectedChecksum));
  return results;
}

void printHeader() {
  std::cout << "bits,shape,target_pct,actual_pct,touches,point_ns";
  for (uint32_t size : {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096}) {
    std::cout << ",n" << size << "_ns,n" << size << "_over_point";
  }
  std::cout << '\n';
}

void runShape(const Options& options, const std::vector<char>& encoded,
              uint8_t bits, std::string_view shape,
              std::span<const double> densities) {
  const uint32_t mask = LinearPack::mask32(bits);
  for (double density : densities) {
    std::vector<uint32_t> touches = shape == "uniform"
        ? makeUniformTouches(options.valueCount, density)
        : makeRunTouches(options.valueCount, density);
    Measurement point = measure(
        [&]() {
          return servicePoint(encoded.data(), bits, mask, touches);
        },
        touches.size(), options.minMs);
    std::vector<BulkResult> bulk = measureBulkSizes(
        encoded, options.valueCount, bits, mask, touches,
        options.minMs, point.checksum);

    const double actualPct =
        100.0 * (double)touches.size() / (double)options.valueCount;
    std::cout << (int32_t)bits << ',' << shape << ','
              << density * 100.0 << ',' << actualPct << ','
              << touches.size() << ',' << point.nsPerTouch;
    for (const BulkResult& result : bulk) {
      std::cout << ',' << result.timing.nsPerTouch
                << ',' << result.timing.nsPerTouch / point.nsPerTouch;
    }
    std::cout << '\n';
  }
}

int run(int argc, char** argv) {
  const Options options = parseOptions(argc, argv);
  constexpr std::array<double, 17> fullDensities = {
      0.0001, 0.0002, 0.0004, 0.0008, 0.0016, 0.0032,
      0.0064, 0.0125, 0.025, 0.05, 0.10, 0.20, 0.30,
      0.40, 0.60, 0.80, 1.0};
  constexpr std::array<double, 4> quickDensities = {
      0.0001, 0.01, 0.10, 1.0};
  const std::span<const double> densities = options.quick
      ? std::span<const double>(quickDensities)
      : std::span<const double>(fullDensities);

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "# values=" << options.valueCount
            << " min_ms=" << options.minMs
            << " samples=" << SAMPLES
            << " uniform=sorted_bernoulli runs=4\n";
  printHeader();
  for (uint8_t bits : {13, 16}) {
    std::vector<char> encoded = makeColumn(options.valueCount, bits);
    runShape(options, encoded, bits, "uniform", densities);
    runShape(options, encoded, bits, "runs", densities);
  }
  return 0;
}

} // namespace
} // namespace solux

int main(int argc, char** argv) {
  try {
    return solux::run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "solux_ord_bulk_bench: " << e.what() << '\n';
    return 1;
  }
}
