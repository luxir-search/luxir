// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/util/DeadlineScheduler.h"

namespace luxir {

class SignalWriter;

enum class InflightKind : uint8_t { SEARCH = 1, UPDATE = 2, ADMIN = 3 };
enum class InflightTransport : uint8_t { INTERNAL = 0, GRPC = 1, HTTP = 2 };
// QUEUED covers admission and waits before execution (a min_commit wait, an
// update batch queued behind its writer); REPLYING a search whose final
// response is being delivered.
enum class InflightPhase : uint8_t { QUEUED = 0, RUNNING = 1, REPLYING = 2 };

const char* inflightName(InflightKind kind);
const char* inflightName(InflightTransport transport);
const char* inflightName(InflightPhase phase);

// One running request, as read back from a registry.
struct InflightRequest {
  int64_t startedMs = 0;   // wall clock, ms since the Unix epoch
  uint64_t thread = 0;     // kernel thread id that last advanced it
  std::string tenant, collection, requestId, operation;
  int32_t maxParallel = 0;
  InflightKind kind = InflightKind::SEARCH;
  InflightTransport transport = InflightTransport::INTERNAL;
  InflightPhase phase = InflightPhase::QUEUED;
};

// What the previous process on a data directory left behind when it did not
// shut down cleanly.
struct UncleanExit {
  // signal, uncaught_exception, oom_kill, memory_limit or unknown.
  std::string cause;
  std::string detail;
  std::string version;
  std::vector<InflightRequest> requests;
  int64_t startedMs = 0;
  int64_t lastSampleMs = 0;    // 0: never sampled
  int64_t detectedMs = 0;      // when this process found the record
  int64_t cgroupBytes = -1;    // -1: unknown
  int64_t cgroupMaxBytes = -1; // -1: unknown or unlimited
  uint64_t residentBytes = 0;
  uint64_t residentPeakBytes = 0;
  uint64_t hostMemoryBytes = 0;
  uint64_t untracked = 0;      // requests running past a full table
  uint32_t pid = 0;

  // Multi-line text for the startup log.
  std::string describe() const;
};

// A fixed table of the node's running requests: search, update and
// administration requests claim a slot when they start and release it when
// they end. Registration takes no lock and allocates nothing; the slots are
// plain bytes and atomics, so the crash handler can print them from a signal
// handler.
//
// With a directory the table lives in a MAP_SHARED file there, so the kernel
// keeps its last state even when the process is killed with SIGKILL (the OOM
// killer). The file header carries a clean-shutdown flag, the crash cause when
// the crash handler saw one, and a memory sample taken every few seconds. The
// next process on the directory reads it before reusing it; when the previous
// run did not shut down cleanly, previousExit() reports what was running and
// the likely cause. The file is versioned and validated field by field: a
// record that does not check out is ignored.
class InflightRegistry {
public:
  struct Header;
  struct Slot;

  struct Options {
    // Empty: anonymous memory, nothing survives the process.
    std::string directory{};
    // This process's cgroup v2 directory, for memory samples and the
    // oom_kill counter; empty when unavailable.
    std::string cgroupDirectory{};
    std::chrono::milliseconds sampleInterval{5000};
  };

private:
  Options options;
  std::string filePath;
  char crashPath[1024] = {};
  Header* header = nullptr;
  Slot* slots = nullptr;
  size_t mappedBytes = 0;
  bool fileBacked = false;
  std::optional<UncleanExit> previous;
  DeadlineScheduler::Slot timer{[this] { sample(); arm(); }};

public:
  static constexpr uint32_t kSlots = 256;
  static constexpr const char* kFileName = "inflight";
  static constexpr const char* kCrashFileName = "crash.txt";

  // Field capacities, including the terminating NUL; longer values truncate.
  static constexpr size_t kTenantBytes = 32;
  static constexpr size_t kCollectionBytes = 64;
  static constexpr size_t kRequestIdBytes = 40;
  static constexpr size_t kOperationBytes = 88;

  struct Request {
    std::string_view tenant{};
    std::string_view collection{};
    std::string_view requestId{};
    std::string_view operation{};
    int32_t maxParallel = 0;
    InflightKind kind = InflightKind::SEARCH;
    InflightTransport transport = InflightTransport::INTERNAL;
    InflightPhase phase = InflightPhase::RUNNING;
  };

  // A claimed slot, released on destruction. Default-constructed or moved-from
  // entries hold nothing. A request that found the table full counts as
  // untracked until its entry is released.
  class Entry {
    friend class InflightRegistry;
    InflightRegistry* registry = nullptr;
    int32_t slot = -1;
    Entry(InflightRegistry* registry, int32_t slot) : registry(registry), slot(slot) {}
  public:
    Entry() = default;
    Entry(Entry&& other) noexcept : registry(other.registry), slot(other.slot) { other.registry = nullptr; }
    Entry& operator=(Entry&& other) noexcept {
      if (this != &other) {
        release();
        registry = other.registry;
        slot = other.slot;
        other.registry = nullptr;
      }
      return *this;
    }
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    ~Entry() { release(); }

    // Moves the request to `phase` and records the calling thread.
    void phase(InflightPhase phase) noexcept;
    void release() noexcept;
    bool tracked() const { return registry && slot >= 0; }
  };

  // Anonymous memory.
  InflightRegistry();
  explicit InflightRegistry(Options options);
  // Marks the shutdown clean.
  ~InflightRegistry();
  InflightRegistry(const InflightRegistry&) = delete;
  InflightRegistry& operator=(const InflightRegistry&) = delete;

  Entry track(const Request& request) noexcept;
  std::vector<InflightRequest> running() const;
  uint64_t untracked() const;
  const std::optional<UncleanExit>& previousExit() const { return previous; }
  // The backing file, or empty for anonymous memory.
  const std::string& path() const { return filePath; }

  // Records process memory into the header now (the timer does it every
  // sampleInterval for a file-backed registry).
  void sample();

  // Leaves the file as SIGKILL would: without a clean-shutdown mark and with
  // this moment's requests. Later changes reach only private memory.
  void abandonForTests();

  // Async-signal-safe: print this registry's running requests, marking those
  // last advanced by `crashingThread`.
  void dump(SignalWriter& out, uint64_t crashingThread) const;
  // Async-signal-safe, over every live registry in the process.
  static void dumpAll(SignalWriter& out, uint64_t crashingThread);
  // Async-signal-safe: note the fatal signal in every file header.
  static void recordFatalSignal(int sig);
  // Notes an uncaught exception's description in every file header.
  static void recordTerminate(const char* text);
  // Async-signal-safe: for every file-backed registry, truncate its crash file,
  // call `writer` with the descriptor, then append that registry's dump.
  using CrashWriter = void (*)(int fd, void* context);
  static void writeCrashFiles(CrashWriter writer, void* context, uint64_t crashingThread);
  // The kernel id of the calling thread; async-signal-safe.
  static uint64_t currentThread();

private:
  void readPrevious(int fd);
  void initialize();
  void arm();
  void unmap();
};

} // namespace luxir
