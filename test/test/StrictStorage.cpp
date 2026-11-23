// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "StrictStorage.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "LuxirTest.h"
#include "luxir/store/Manifest.h"

#if defined(LUXIR_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace luxir::test {

StrictStorageCounters::FileStats StrictStorageCounters::fileStats(std::string_view name) const {
  std::lock_guard lock(mutex);
  auto it = files.find(name);
  return it == files.end() ? FileStats{} : it->second;
}

uint64_t StrictStorageCounters::totalRounds() const {
  std::lock_guard lock(mutex);
  return rounds;
}

StrictStorageCounters::LiveStats StrictStorageCounters::liveStats() const {
  std::lock_guard lock(mutex);
  return live;
}

void StrictStorageCounters::logSummary() const {
  std::lock_guard lock(mutex);
  uint64_t acquires = 0, metadata = 0, uniqueParts = 0, wholeFiles = 0, sizes = 0, batches = 0;
  for (const auto& [name, file] : files) {
    acquires += file.acquires;
    batches += file.batches;
    metadata += file.metadataAcquires;
    uniqueParts += file.parts.size();
    wholeFiles += file.wholeFileAccesses;
    sizes += file.sizeRequests;
  }
  LOG_INFO("strict: acquires={} (metadata={}), rounds={} (batches={}), unique parts={}, peak live holds={}, "
           "peak live bytes={}, whole-file accesses={}, size requests={}",
           acquires, metadata, rounds, batches, uniqueParts, live.peakHolds, live.peakBytes, wholeFiles, sizes);
}

StrictLayer::~StrictLayer() {
  counters->logSummary();
}

void StrictStorageCounters::wholeFile(std::string_view name) {
  std::lock_guard lock(mutex);
  ++files[std::string(name)].wholeFileAccesses;
}

void StrictStorageCounters::sizeRequested(std::string_view name) {
  std::lock_guard lock(mutex);
  ++files[std::string(name)].sizeRequests;
}

void StrictStorageCounters::round(std::string_view name, bool batch) {
  std::lock_guard lock(mutex);
  auto& file = files[std::string(name)];
  file.lastRound = ++rounds;
  if (!file.rounds++) file.firstRound = rounds;
  if (batch) ++file.batches;
}

uint8_t StrictStorageCounters::acquired(std::string_view name, ByteRange range) {
  std::lock_guard lock(mutex);
  auto& file = files[std::string(name)];
  file.parts.emplace(range.offset, range.length);
  ++file.acquires;
  if (range.readClass == ReadClass::METADATA) {
    file.metadataParts.emplace(range.offset, range.length);
    ++file.metadataAcquires;
  }
  file.acquiredBytes += range.length;
  ++live.holds;
  live.bytes += range.length;
  live.peakHolds = std::max(live.peakHolds, live.holds);
  live.peakBytes = std::max(live.peakBytes, live.bytes);
  return ++pattern;
}

void StrictStorageCounters::released(uint64_t length) noexcept {
  std::lock_guard lock(mutex);
  --live.holds;
  live.bytes -= length;
}

namespace {

class StrictFileState : public HoldOwner {
  // One alignment unit before the range, up to one for its offset, and a
  // guard after the readable slack. Round up to end on a shadow boundary.
  static constexpr size_t overhead = 3 * kHoldAlign + kReadSlack;
  std::shared_ptr<StrictStorageCounters> counters;
  std::string name;
  bool denyWhole;
  std::atomic<uint64_t> liveHolds{0};

  static size_t allocationSize(size_t length) {
    return (length + overhead + kHoldAlign - 1) & ~(kHoldAlign - 1);
  }

public:
  StrictFileState(std::shared_ptr<StrictStorageCounters> counters, std::string_view name, StrictWholeFiles mode)
      : counters(std::move(counters)), name(name),
        denyWhole(mode == StrictWholeFiles::DENY && Manifest::segmentDataFile(name)) {}

  // A hold must not outlive its InputFile: its release would run on freed state.
  ~StrictFileState() {
    if (auto live = liveHolds.load()) {
      std::fprintf(stderr, "strict: %llu holds outlive their InputFile %s\n", (unsigned long long)live, name.c_str());
      std::abort();
    }
  }

  void sizeRequested() { counters->sizeRequested(name); }
  void round(bool batch) {
    if (counters->takeInjectedFailure()) throw FileIOException("strict: injected acquire failure on " + name);
    counters->round(name, batch);
  }

  void wholeFile() {
    counters->wholeFile(name);
    if (denyWhole) throw FileIOException("strict: whole-file access to segment data file " + name);
  }

  HeldRange copy(const HeldRange& source, ByteRange range, size_t slack) {
    if (range.length > SIZE_MAX - overhead - (kHoldAlign - 1)) throw std::bad_array_new_length();
    size_t total = allocationSize(range.length);
    static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= kHoldAlign);
    auto buffer = std::make_unique_for_overwrite<char[]>(total);
    size_t prefix = kHoldAlign + range.offset % kHoldAlign;
    char* data = buffer.get() + prefix;
    uint8_t pattern = counters->acquired(name, range);
    std::memset(buffer.get(), pattern, prefix);
    if (range.length) std::memcpy(data, source.getData(), range.length);
    std::memset(data + range.length, pattern, total - prefix - range.length);
#if defined(LUXIR_ASAN)
    // ASan cannot poison the beginning of a shadow granule on its own.
    constexpr size_t granule = 8;
    ASAN_POISON_MEMORY_REGION(buffer.get(), prefix - prefix % granule);
    size_t readableEnd = prefix + range.length + slack;
    ASAN_POISON_MEMORY_REGION(buffer.get() + readableEnd, total - readableEnd);
#else
    (void)slack;
#endif
    liveHolds.fetch_add(1, std::memory_order_relaxed);
    return {data, range.length, this, (uintptr_t)buffer.release()};
  }

  void release(const HeldRange& hold) noexcept override {
    auto* buffer = (char*)hold.getToken();
#if defined(LUXIR_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(buffer, allocationSize(hold.getLength()));
#endif
    delete[] buffer;
    counters->released(hold.getLength());
    liveHolds.fetch_sub(1, std::memory_order_relaxed);
  }
};

class StrictInputFile : public InputFile {
  // Preserve the shared_ptr as returned, including any aliasing lease.
  std::shared_ptr<InputFile> delegate;
  std::shared_ptr<StrictFileState> state;

  static size_t slack(ByteRange range, uint64_t fileSize) {
    return std::min<uint64_t>(kReadSlack, fileSize - range.offset - range.length);
  }

public:
  StrictInputFile(std::shared_ptr<InputFile> inner, std::shared_ptr<StrictFileState> state)
      : delegate(std::move(inner)), state(std::move(state)) {}

  size_t size() override {
    state->sizeRequested();
    return delegate->size();
  }
  bool holdsFree() const override { return false; }

  using InputFile::acquire;

  HeldRange acquire(ByteRange range) override {
    auto fileSize = delegate->size();
    checkRange(range, fileSize);
    if (range.offset == 0 && range.length == fileSize) state->wholeFile();
    state->round(false);
    auto source = delegate->acquire(range);
    return state->copy(source, range, slack(range, fileSize));
  }

  // Forwards the batch whole, so the delegate sees it as one round.
  void acquire(std::span<const ByteRange> ranges, AcquireDone done) override {
    auto fileSize = delegate->size();
    for (auto range : ranges) {
      checkRange(range, fileSize);
      if (range.offset == 0 && range.length == fileSize) state->wholeFile();
    }
    state->round(true);
    std::vector<ByteRange> requested(ranges.begin(), ranges.end());
    delegate->acquire(ranges, [state = state, fileSize, requested = std::move(requested), done = std::move(done)](
                                  std::vector<HeldRange>&& sources, std::exception_ptr failure) {
      std::vector<HeldRange> holds;
      if (!failure) {
        try {
          if (sources.size() != requested.size()) throw FileIOException("strict: batch returned the wrong number of holds");
          holds.reserve(sources.size());
          for (size_t i = 0; i < sources.size(); ++i) {
            holds.push_back(state->copy(sources[i], requested[i], slack(requested[i], fileSize)));
          }
        } catch (...) {
          holds.clear();
          failure = std::current_exception();
        }
      }
      sources.clear();
      done(std::move(holds), std::move(failure));
    });
  }

  void prefetch(size_t offset, size_t length) override { delegate->prefetch(offset, length); }
};

class StrictChecksumReader : public ChecksumReader {
  std::shared_ptr<ChecksumReader> delegate;
  std::shared_ptr<StrictFileState> state;

public:
  StrictChecksumReader(std::shared_ptr<ChecksumReader> inner, std::shared_ptr<StrictFileState> state)
      : delegate(std::move(inner)), state(std::move(state)) {}

  uint64_t size() const override { return delegate->size(); }

  std::shared_ptr<InputFile> map() override {
    auto file = delegate->map();
    if (!file) return {};
    return std::make_shared<StrictInputFile>(std::move(file), state);
  }

  FileCheck checksum(const std::function<void(size_t)>& progress) override {
    return delegate->checksum(progress);
  }
};

} // namespace

std::shared_ptr<InputFile> StrictDirectory::openFile(std::string_view name, bool expectSynced, uint64_t size) {
  auto file = delegate->openFile(name, expectSynced, size);
  if (!file) return {};
  if (size && size != file->size()) throw FileIOException("strict: open size differs from file " + std::string(name));
  return std::make_shared<StrictInputFile>(std::move(file), std::make_shared<StrictFileState>(counters, name, mode));
}

std::shared_ptr<ChecksumReader> StrictDirectory::openChecksum(std::string_view name, bool direct) {
  auto reader = delegate->openChecksum(name, direct);
  if (!reader) return {};
  return std::make_shared<StrictChecksumReader>(std::move(reader), std::make_shared<StrictFileState>(counters, name, mode));
}

} // namespace luxir::test
