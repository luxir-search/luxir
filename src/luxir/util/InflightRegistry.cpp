// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/InflightRegistry.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#else
#include <pthread.h>
#endif

#include "luxir/BuildInfo.h"
#include "luxir/util/SignalWriter.h"
#include "luxir/util/log.h"

namespace luxir {

// The file: one Header, then kSlots Slots. Every field is fixed width and
// native endian; the version changes with any layout change.
struct InflightRegistry::Header {
  char magic[8];
  uint32_t version;
  uint32_t headerBytes;
  uint32_t slotBytes;
  uint32_t slotCount;
  uint32_t pid;
  uint32_t state;        // kRunning or kClean
  int32_t signal;        // the fatal signal the crash handler saw, else 0
  uint32_t terminated;   // 1: std::terminate ran (exception holds the text)
  int64_t startedMs;
  int64_t sampledMs;     // 0 until the first memory sample
  uint64_t residentBytes;
  uint64_t residentPeakBytes;
  int64_t cgroupBytes;
  int64_t cgroupMaxBytes;
  uint64_t hostMemoryBytes;
  uint64_t oomKills;       // the cgroup's memory.events oom_kill at the sample
  uint64_t oomKillsKnown;  // 1 when oomKills was read
  uint64_t untracked;
  char buildVersion[48];
  char cgroup[160];
  char exception[200];
};

struct InflightRegistry::Slot {
  uint64_t word;         // epoch << 2 | kFree / kWriting / kLive
  int64_t startedMs;
  uint64_t thread;
  int32_t maxParallel;
  uint8_t kind;
  uint8_t transport;
  uint8_t phase;
  uint8_t reserved;
  char tenant[kTenantBytes];
  char collection[kCollectionBytes];
  char requestId[kRequestIdBytes];
  char operation[kOperationBytes];
};

namespace {

using Header = InflightRegistry::Header;
using Slot = InflightRegistry::Slot;

constexpr char kMagic[8] = {'L', 'X', 'I', 'N', 'F', 'L', 'T', '\0'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRunning = 1;
constexpr uint32_t kClean = 2;
constexpr uint64_t kFree = 0;
constexpr uint64_t kWriting = 1;
constexpr uint64_t kLive = 2;
constexpr size_t kFileBytes = sizeof(Header) + InflightRegistry::kSlots * sizeof(Slot);
static_assert(sizeof(Slot) == 256);
static_assert(std::is_trivially_copyable_v<Header> && std::is_trivially_copyable_v<Slot>);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free && std::atomic_ref<uint8_t>::is_always_lock_free);

// Every registry in the process, for the crash handler. Tests run many nodes.
constexpr int kMaxRegistries = 64;
std::atomic<InflightRegistry*> registries[kMaxRegistries];

template <class T>
std::atomic_ref<T> ref(T& value) { return std::atomic_ref<T>(value); }
template <class T>
T load(const T& value) { return std::atomic_ref<T>(const_cast<T&>(value)).load(std::memory_order_relaxed); }

int64_t nowMs() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);  // async-signal-safe
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void copyField(char* dst, size_t cap, std::string_view src) {
  size_t n = std::min(src.size(), cap - 1);
  memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

std::string field(const char* src, size_t cap) { return std::string(src, strnlen(src, cap)); }
bool terminated(const char* src, size_t cap) { return memchr(src, '\0', cap) != nullptr; }

// A seqlock read: the slot's bytes, if it held one request throughout.
bool readSlot(const Slot& slot, Slot& copy) {
  uint64_t before = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(slot.word)).load(std::memory_order_acquire);
  if ((before & 3) != kLive) return false;
  memcpy(&copy, &slot, sizeof(Slot));
  std::atomic_thread_fence(std::memory_order_acquire);
  return load(slot.word) == before;
}

InflightRequest toRequest(const Slot& slot) {
  InflightRequest request;
  request.kind = (InflightKind)slot.kind;
  request.transport = (InflightTransport)slot.transport;
  request.phase = (InflightPhase)slot.phase;
  request.maxParallel = slot.maxParallel;
  request.thread = slot.thread;
  request.startedMs = slot.startedMs;
  request.tenant = field(slot.tenant, sizeof(slot.tenant));
  request.collection = field(slot.collection, sizeof(slot.collection));
  request.requestId = field(slot.requestId, sizeof(slot.requestId));
  request.operation = field(slot.operation, sizeof(slot.operation));
  return request;
}

const char* signalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return nullptr;
  }
}

std::string isoTime(int64_t ms) {
  if (ms <= 0) return "unknown";
  time_t seconds = (time_t)(ms / 1000);
  tm utc{};
  gmtime_r(&seconds, &utc);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

std::string mib(uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

// Small procfs and cgroupfs reads; absent files read as nullopt.
std::optional<std::string> readText(const std::string& path) {
  FILE* file = fopen(path.c_str(), "re");
  if (!file) return std::nullopt;
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0 && text.size() < 64 * 1024) text.append(buf, n);
  fclose(file);
  return text;
}

// The number after `key` at the start of a line ("VmRSS:\t 123 kB", "oom_kill 2").
std::optional<uint64_t> keyed(const std::string& text, std::string_view key) {
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + at, end - at);
    if (line.starts_with(key) && line.size() > key.size() && (line[key.size()] == ' ' || line[key.size()] == '\t')) {
      return strtoull(std::string(line.substr(key.size())).c_str(), nullptr, 10);
    }
    at = end + 1;
  }
  return std::nullopt;
}

// memory.current / memory.max: a number, or "max" (-1).
int64_t cgroupValue(const std::string& directory, const char* name) {
  if (directory.empty()) return -1;
  auto text = readText(directory + "/" + name);
  if (!text || text->empty()) return -1;
  if (text->starts_with("max")) return -1;
  return (int64_t)strtoll(text->c_str(), nullptr, 10);
}

std::optional<uint64_t> oomKills(const std::string& cgroupDirectory) {
  if (cgroupDirectory.empty()) return std::nullopt;
  auto text = readText(cgroupDirectory + "/memory.events");
  return text ? keyed(*text, "oom_kill") : std::nullopt;
}

// Reads a previous file image. Returns nullopt for a clean shutdown; sets
// `error` when the image does not validate.
std::optional<UncleanExit> parseImage(const char* data, size_t size, const std::string& cgroupDirectory,
                                      std::string& error) {
  if (size != kFileBytes) { error = "unexpected size " + std::to_string(size); return std::nullopt; }
  Header header;
  memcpy(&header, data, sizeof(header));
  if (memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) { error = "bad magic"; return std::nullopt; }
  if (header.version != kVersion) { error = "unsupported version " + std::to_string(header.version); return std::nullopt; }
  if (header.headerBytes != sizeof(Header) || header.slotBytes != sizeof(Slot) || header.slotCount != InflightRegistry::kSlots) {
    error = "unexpected layout";
    return std::nullopt;
  }
  if (header.state != kRunning && header.state != kClean) { error = "bad state"; return std::nullopt; }
  if (!terminated(header.buildVersion, sizeof(header.buildVersion)) || !terminated(header.cgroup, sizeof(header.cgroup))
      || !terminated(header.exception, sizeof(header.exception)) || header.terminated > 1 || header.oomKillsKnown > 1
      || header.signal < 0 || header.signal > 64) {
    error = "bad header field";
    return std::nullopt;
  }
  UncleanExit exit;
  for (uint32_t i = 0; i < InflightRegistry::kSlots; i++) {
    Slot slot;
    memcpy(&slot, data + sizeof(Header) + i * sizeof(Slot), sizeof(slot));
    uint64_t state = slot.word & 3;
    if (state == kFree || state == kWriting) continue;  // a claim cut short holds nothing yet
    if (state != kLive || slot.kind < 1 || slot.kind > 3 || slot.transport > 2 || slot.phase > 2
        || !terminated(slot.tenant, sizeof(slot.tenant)) || !terminated(slot.collection, sizeof(slot.collection))
        || !terminated(slot.requestId, sizeof(slot.requestId)) || !terminated(slot.operation, sizeof(slot.operation))) {
      error = "bad slot " + std::to_string(i);
      return std::nullopt;
    }
    exit.requests.push_back(toRequest(slot));
  }
  if (header.state == kClean) return std::nullopt;
  std::ranges::sort(exit.requests, {}, &InflightRequest::startedMs);
  exit.pid = header.pid;
  exit.version = field(header.buildVersion, sizeof(header.buildVersion));
  exit.startedMs = header.startedMs;
  exit.lastSampleMs = header.sampledMs;
  exit.residentBytes = header.residentBytes;
  exit.residentPeakBytes = header.residentPeakBytes;
  exit.cgroupBytes = header.cgroupBytes;
  exit.cgroupMaxBytes = header.cgroupMaxBytes;
  exit.hostMemoryBytes = header.hostMemoryBytes;
  exit.untracked = header.untracked;
  exit.detectedMs = nowMs();

  // The likeliest cause, from the strongest evidence down.
  auto exception = field(header.exception, sizeof(header.exception));
  auto now = header.oomKillsKnown && field(header.cgroup, sizeof(header.cgroup)) == cgroupDirectory
      ? oomKills(cgroupDirectory) : std::nullopt;
  auto near = [](uint64_t used, int64_t limit) { return limit > 0 && used >= (uint64_t)limit / 10 * 9; };
  if (header.terminated) {
    exit.cause = "uncaught_exception";
    exit.detail = exception;
  } else if (header.signal != 0) {
    exit.cause = "signal";
    const char* name = signalName(header.signal);
    exit.detail = name ? name : "signal " + std::to_string(header.signal);
  } else if (now && *now > header.oomKills) {
    exit.cause = "oom_kill";
    exit.detail = "cgroup memory.events oom_kill rose from " + std::to_string(header.oomKills) + " to " + std::to_string(*now);
  } else if (header.sampledMs && (near(header.cgroupBytes < 0 ? 0 : (uint64_t)header.cgroupBytes, header.cgroupMaxBytes)
                                  || near(header.residentBytes, (int64_t)header.hostMemoryBytes))) {
    exit.cause = "memory_limit";
    exit.detail = header.cgroupMaxBytes > 0 && near((uint64_t)std::max<int64_t>(header.cgroupBytes, 0), header.cgroupMaxBytes)
        ? "cgroup memory " + mib((uint64_t)header.cgroupBytes) + " of " + mib((uint64_t)header.cgroupMaxBytes) + " at the last sample"
        : "resident " + mib(header.residentBytes) + " of " + mib(header.hostMemoryBytes) + " host memory at the last sample";
  } else {
    exit.cause = "unknown";
    exit.detail = "killed without a report (SIGKILL, an OOM kill outside this cgroup, or power loss)";
  }
  return exit;
}

void writeRequest(SignalWriter& out, const Slot& slot, int64_t now, uint64_t crashingThread) {
  out.str("  ").str(inflightName((InflightKind)slot.kind)).str(" ").str(inflightName((InflightTransport)slot.transport))
     .str(" ").str(inflightName((InflightPhase)slot.phase))
     .str(" tenant=").field(slot.tenant, sizeof(slot.tenant))
     .str(" collection=").field(slot.collection, sizeof(slot.collection));
  if (slot.requestId[0]) out.str(" request_id=").field(slot.requestId, sizeof(slot.requestId));
  if ((InflightKind)slot.kind == InflightKind::SEARCH) out.str(" max_parallel=").dec(slot.maxParallel);
  out.str(" age_ms=").dec(now - slot.startedMs).str(" tid=").dec((long long)slot.thread);
  if (slot.operation[0]) out.str(" op=").field(slot.operation, sizeof(slot.operation));
  if (crashingThread && slot.thread == crashingThread) out.str(" [crashing thread]");
  out.str("\n");
}

} // namespace

const char* inflightName(InflightKind kind) {
  switch (kind) {
    case InflightKind::SEARCH: return "search";
    case InflightKind::UPDATE: return "update";
    case InflightKind::ADMIN: return "admin";
  }
  return "?";
}

const char* inflightName(InflightTransport transport) {
  switch (transport) {
    case InflightTransport::INTERNAL: return "internal";
    case InflightTransport::GRPC: return "grpc";
    case InflightTransport::HTTP: return "http";
  }
  return "?";
}

const char* inflightName(InflightPhase phase) {
  switch (phase) {
    case InflightPhase::QUEUED: return "queued";
    case InflightPhase::RUNNING: return "running";
    case InflightPhase::REPLYING: return "replying";
  }
  return "?";
}

std::string UncleanExit::describe() const {
  std::string text = "The previous run on this data directory did not shut down cleanly: " + cause;
  if (!detail.empty()) text += " (" + detail + ")";
  text += "\n  pid " + std::to_string(pid) + ", " + version + ", started " + isoTime(startedMs);
  if (lastSampleMs) {
    text += "; at the last memory sample (" + isoTime(lastSampleMs) + "): resident " + mib(residentBytes)
         + ", peak " + mib(residentPeakBytes);
    if (cgroupBytes >= 0) text += ", cgroup " + mib((uint64_t)cgroupBytes) + " of " + (cgroupMaxBytes > 0 ? mib((uint64_t)cgroupMaxBytes) : "unlimited");
  }
  if (requests.empty() && !untracked) {
    text += "\n  no requests were running";
    return text;
  }
  std::map<std::string, int> collections;
  for (const auto& request : requests) collections[request.tenant + "/" + request.collection]++;
  text += "\n  " + std::to_string(requests.size() + untracked) + " requests were running";
  if (untracked) text += " (" + std::to_string(untracked) + " not recorded: the table was full)";
  text += "; collections:";
  for (const auto& [name, count] : collections) text += " " + name + " (" + std::to_string(count) + ")";
  for (const auto& request : requests) {
    text += "\n    " + std::string(inflightName(request.kind)) + " " + inflightName(request.transport) + " "
         + inflightName(request.phase) + " " + request.tenant + "/" + request.collection;
    if (!request.requestId.empty()) text += " request_id=" + request.requestId;
    if (!request.operation.empty()) text += " op=" + request.operation;
    text += " started " + isoTime(request.startedMs);
  }
  return text;
}

uint64_t InflightRegistry::currentThread() {
#if defined(__linux__)
  return (uint64_t)::syscall(SYS_gettid);
#else
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return id;
#endif
}

namespace {
// The kernel id is a syscall; cache it per thread for the hot path.
uint64_t cachedThread() {
  thread_local uint64_t id = InflightRegistry::currentThread();
  return id;
}
} // namespace

InflightRegistry::InflightRegistry() : InflightRegistry(Options()) {}

InflightRegistry::InflightRegistry(Options options) : options(std::move(options)) {
  if (!this->options.directory.empty()) {
    filePath = this->options.directory + "/" + kFileName;
    int fd = ::open(filePath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      LOG_WARN("Cannot open {} ({}): running requests will not be reported after a crash", filePath, strerror(errno));
    } else {
      readPrevious(fd);
      void* memory = MAP_FAILED;
      if (ftruncate(fd, (off_t)kFileBytes) == 0) memory = mmap(nullptr, kFileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (memory == MAP_FAILED) {
        LOG_WARN("Cannot map {} ({}): running requests will not be reported after a crash", filePath, strerror(errno));
      } else {
        header = (Header*)memory;
        fileBacked = true;
        snprintf(crashPath, sizeof(crashPath), "%s/%s", this->options.directory.c_str(), kCrashFileName);
      }
      ::close(fd);
    }
  }
  if (!header) {
    filePath.clear();
    void* memory = mmap(nullptr, kFileBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::runtime_error("cannot allocate the in-flight request table");
    header = (Header*)memory;
  }
  mappedBytes = kFileBytes;
  slots = (Slot*)((char*)header + sizeof(Header));
  initialize();
  for (auto& registry : registries) {
    InflightRegistry* empty = nullptr;
    if (registry.compare_exchange_strong(empty, this)) break;
  }
  if (fileBacked) {
    sample();
    arm();
  }
}

InflightRegistry::~InflightRegistry() {
  DeadlineScheduler::global().detach(timer);
  if (!header) return;
  ref(header->state).store(kClean, std::memory_order_release);
  if (fileBacked) msync(header, mappedBytes, MS_SYNC);
  unmap();
}

void InflightRegistry::abandonForTests() {
  DeadlineScheduler::global().detach(timer);
  if (!fileBacked) return;
  // Swap the shared mapping for private memory with the same contents: the
  // file keeps this moment's state, and later releases and the clean-shutdown
  // mark only reach the copy.
  std::vector<char> image((char*)header, (char*)header + mappedBytes);
  void* memory = mmap(header, mappedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (memory == MAP_FAILED) throw std::runtime_error("cannot detach the in-flight request table");
  memcpy(memory, image.data(), mappedBytes);
  fileBacked = false;
  crashPath[0] = '\0';
}

void InflightRegistry::unmap() {
  for (auto& registry : registries) {
    InflightRegistry* self = this;
    if (registry.compare_exchange_strong(self, nullptr)) break;
  }
  munmap(header, mappedBytes);
  header = nullptr;
  slots = nullptr;
}

void InflightRegistry::readPrevious(int fd) {
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size == 0) return;  // a new file
  std::vector<char> image((size_t)std::min<off_t>(st.st_size, (off_t)(2 * kFileBytes)));
  size_t done = 0;
  while (done < image.size()) {
    ssize_t n = pread(fd, image.data() + done, image.size() - done, (off_t)done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += (size_t)n;
  }
  std::string error;
  if (done < image.size()) error = std::string("read failed: ") + strerror(errno);
  else previous = parseImage(image.data(), (size_t)st.st_size, options.cgroupDirectory, error);
  if (!error.empty()) LOG_WARN("Ignoring {}: {}; cannot tell what the previous run was doing", filePath, error);
}

void InflightRegistry::initialize() {
  memset((void*)header, 0, mappedBytes);
  memcpy(header->magic, kMagic, sizeof(kMagic));
  header->version = kVersion;
  header->headerBytes = sizeof(Header);
  header->slotBytes = sizeof(Slot);
  header->slotCount = kSlots;
  header->pid = (uint32_t)getpid();
  header->startedMs = nowMs();
  header->cgroupBytes = -1;
  header->cgroupMaxBytes = -1;
  const auto& build = buildInfo();
  snprintf(header->buildVersion, sizeof(header->buildVersion), "Luxir %.*s commit %.12s",
           (int)build.version.size(), build.version.data(), std::string(build.sourceCommit).c_str());
  copyField(header->cgroup, sizeof(header->cgroup), options.cgroupDirectory);
  ref(header->state).store(kRunning, std::memory_order_release);
}

void InflightRegistry::arm() {
  DeadlineScheduler::global().arm(timer, DeadlineScheduler::Clock::now() + options.sampleInterval);
}

void InflightRegistry::sample() {
  if (!header) return;
#if defined(__linux__)
  if (auto status = readText("/proc/self/status")) {
    ref(header->residentBytes).store(keyed(*status, "VmRSS:").value_or(0) * 1024, std::memory_order_relaxed);
    ref(header->residentPeakBytes).store(keyed(*status, "VmHWM:").value_or(0) * 1024, std::memory_order_relaxed);
  }
  if (!header->hostMemoryBytes) {
    if (auto meminfo = readText("/proc/meminfo"))
      ref(header->hostMemoryBytes).store(keyed(*meminfo, "MemTotal:").value_or(0) * 1024, std::memory_order_relaxed);
  }
  const auto& cgroup = options.cgroupDirectory;
  ref(header->cgroupBytes).store(cgroupValue(cgroup, "memory.current"), std::memory_order_relaxed);
  ref(header->cgroupMaxBytes).store(cgroupValue(cgroup, "memory.max"), std::memory_order_relaxed);
  auto kills = oomKills(cgroup);
  ref(header->oomKills).store(kills.value_or(0), std::memory_order_relaxed);
  ref(header->oomKillsKnown).store(kills ? 1 : 0, std::memory_order_relaxed);
#endif
  ref(header->sampledMs).store(nowMs(), std::memory_order_release);
}

InflightRegistry::Entry InflightRegistry::track(const Request& request) noexcept {
  thread_local uint32_t hint = (uint32_t)(cachedThread() * 2654435761u);
  for (uint32_t i = 0; i < kSlots; i++) {
    uint32_t index = (hint + i) % kSlots;
    Slot& slot = slots[index];
    uint64_t word = ref(slot.word).load(std::memory_order_relaxed);
    if ((word & 3) != kFree) continue;
    uint64_t epoch = (word >> 2) + 1;
    if (!ref(slot.word).compare_exchange_strong(word, epoch << 2 | kWriting, std::memory_order_acquire)) continue;
    slot.startedMs = nowMs();
    slot.thread = cachedThread();
    slot.maxParallel = request.maxParallel;
    slot.kind = (uint8_t)request.kind;
    slot.transport = (uint8_t)request.transport;
    slot.phase = (uint8_t)request.phase;
    copyField(slot.tenant, sizeof(slot.tenant), request.tenant);
    copyField(slot.collection, sizeof(slot.collection), request.collection);
    copyField(slot.requestId, sizeof(slot.requestId), request.requestId);
    copyField(slot.operation, sizeof(slot.operation), request.operation);
    ref(slot.word).store(epoch << 2 | kLive, std::memory_order_release);
    hint = index + 1;
    return Entry(this, (int32_t)index);
  }
  ref(header->untracked).fetch_add(1, std::memory_order_relaxed);
  return Entry(this, -1);
}

void InflightRegistry::Entry::phase(InflightPhase phase) noexcept {
  if (!tracked()) return;
  Slot& s = registry->slots[slot];
  ref(s.thread).store(cachedThread(), std::memory_order_relaxed);
  ref(s.phase).store((uint8_t)phase, std::memory_order_relaxed);
}

void InflightRegistry::Entry::release() noexcept {
  if (!registry) return;
  if (slot >= 0) {
    auto word = ref(registry->slots[slot].word);
    word.store(word.load(std::memory_order_relaxed) & ~(uint64_t)3, std::memory_order_release);
  } else {
    ref(registry->header->untracked).fetch_sub(1, std::memory_order_relaxed);
  }
  registry = nullptr;
}

std::vector<InflightRequest> InflightRegistry::running() const {
  std::vector<InflightRequest> result;
  for (uint32_t i = 0; i < kSlots; i++) {
    Slot copy;
    if (readSlot(slots[i], copy)) result.push_back(toRequest(copy));
  }
  std::ranges::sort(result, {}, &InflightRequest::startedMs);
  return result;
}

uint64_t InflightRegistry::untracked() const { return load(header->untracked); }

void InflightRegistry::dump(SignalWriter& out, uint64_t crashingThread) const {
  if (!header) return;
  int64_t now = nowMs();
  int count = 0;
  for (uint32_t i = 0; i < kSlots; i++) {
    Slot copy;
    if (readSlot(slots[i], copy)) count++;
  }
  out.str("Running requests: ").dec(count);
  if (uint64_t more = load(header->untracked)) out.str(" (+").dec((long long)more).str(" untracked)");
  if (fileBacked) out.str(", recorded in ").str(filePath.c_str());
  out.str("\n");
  for (uint32_t i = 0; i < kSlots; i++) {
    Slot copy;
    if (readSlot(slots[i], copy)) writeRequest(out, copy, now, crashingThread);
  }
  if (load(header->terminated)) out.str("  uncaught exception: ").field(header->exception, sizeof(header->exception)).str("\n");
  if (int64_t sampled = load(header->sampledMs)) {
    out.str("  memory ").dec(now - sampled).str(" ms ago: resident ").dec((long long)load(header->residentBytes))
       .str(" bytes, peak ").dec((long long)load(header->residentPeakBytes)).str(" bytes");
    if (int64_t cgroup = load(header->cgroupBytes); cgroup >= 0) {
      out.str(", cgroup ").dec(cgroup).str(" of ");
      if (int64_t max = load(header->cgroupMaxBytes); max > 0) out.dec(max).str(" bytes");
      else out.str("unlimited");
    }
    out.str("\n");
  }
}

void InflightRegistry::dumpAll(SignalWriter& out, uint64_t crashingThread) {
  for (auto& registry : registries) {
    if (auto* r = registry.load(std::memory_order_acquire)) r->dump(out, crashingThread);
  }
}

void InflightRegistry::recordFatalSignal(int sig) {
  for (auto& registry : registries) {
    auto* r = registry.load(std::memory_order_acquire);
    if (r && r->header && !load(r->header->signal)) ref(r->header->signal).store(sig, std::memory_order_relaxed);
  }
}

void InflightRegistry::recordTerminate(const char* text) {
  for (auto& registry : registries) {
    auto* r = registry.load(std::memory_order_acquire);
    if (!r || !r->header) continue;
    copyField(r->header->exception, sizeof(r->header->exception), text);
    ref(r->header->terminated).store(1, std::memory_order_release);
  }
}

void InflightRegistry::writeCrashFiles(CrashWriter writer, void* context, uint64_t crashingThread) {
  for (auto& registry : registries) {
    auto* r = registry.load(std::memory_order_acquire);
    if (!r || !r->fileBacked || !r->crashPath[0]) continue;
    int fd = ::open(r->crashPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) continue;
    writer(fd, context);
    {
      SignalWriter out(fd);
      r->dump(out, crashingThread);
    }
    ::close(fd);
  }
}

} // namespace luxir
