// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/DateTime.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <absl/time/civil_time.h>
#include <absl/time/time.h>

namespace luxir {
namespace datetime_detail {
namespace {

// Gregorian calendars, and so POSIX-style recurring zone rules, repeat every
// 400 years.
constexpr int64_t kSecsPer400Years = 146097LL * 86400;
const absl::CivilSecond kCivilEpoch{};

int64_t floorDiv(int64_t value, int64_t divisor) {
  int64_t q = value / divisor;
  return q - (value % divisor < 0);
}

int64_t clampMillis(__int128 seconds) {
  return (int64_t)std::clamp(seconds * 1000, (__int128)kMinEpochMs, (__int128)kMaxEpochMs + 1);
}

// An offset-changing transition: its instant and the civil times there under
// the old and the new offset (seconds since 1970).
struct Transition {
  int64_t at;
  int64_t civilBefore;
  int64_t civilAfter;
};

} // namespace

// cctz tabulates a zone's recurring (TZif footer) rule for 400 years past its
// last explicit change and maps later instants back by whole cycles in At(),
// but its transition enumeration stops at the table. A cyclic zone's lookups
// past the last tabulated transition use the same mapping.
struct IanaZoneRules {
  absl::TimeZone tz;
  bool cyclic = false;
  int64_t last = 0;  // last tabulated transition (unix seconds)

  explicit IanaZoneRules(absl::TimeZone tz) : tz(tz) {
    absl::TimeZone::CivilTransition t;
    if (!tz.PrevTransition(absl::InfiniteFuture(), &t)) return;
    last = absl::ToUnixSeconds(tz.At(t.to).trans);
    // Probe cctz's own mode, not the data: past an unextended table At()
    // keeps the offset after the last transition; an extended one maps a
    // cycle later back to the offset before it.
    int64_t before = offsetAt(last - 1);
    cyclic = before != offsetAt(last) && offsetAt(last + kSecsPer400Years - 1) == before;
  }

  int64_t offsetAt(int64_t t) const {
    return tz.At(absl::FromUnixSeconds(t)).offset;
  }

  // First transition after t, given offset, the offset in force at t.
  std::optional<Transition> next(int64_t t, int64_t offset) const {
    if (cyclic && t >= last) {
      int64_t shift = ((t - last) / kSecsPer400Years + 1) * kSecsPer400Years;
      auto r = next(t - shift, offset);
      if (r) *r = {r->at + shift, r->civilBefore + shift, r->civilAfter + shift};
      return r;
    }
    absl::TimeZone::CivilTransition c;
    for (absl::Time from = absl::FromUnixSeconds(t); tz.NextTransition(from, &c);) {
      Transition r{(c.from - kCivilEpoch) - offset, c.from - kCivilEpoch, c.to - kCivilEpoch};
      if (r.civilBefore != r.civilAfter) return r;
      from = absl::FromUnixSeconds(r.at);  // abbreviation or DST-flag change only
    }
    return std::nullopt;
  }

  // Last transition at or before t, given offset, the offset in force at t.
  std::optional<Transition> previous(int64_t t, int64_t offset) const {
    if (cyclic && t >= last) {
      int64_t shift = ((t - last) / kSecsPer400Years + 1) * kSecsPer400Years;
      auto r = previous(t - shift, offset);
      if (r) *r = {r->at + shift, r->civilBefore + shift, r->civilAfter + shift};
      return r;
    }
    absl::TimeZone::CivilTransition c;
    for (absl::Time before = absl::FromUnixSeconds(t + 1); tz.PrevTransition(before, &c);) {
      Transition r{(c.to - kCivilEpoch) - offset, c.from - kCivilEpoch, c.to - kCivilEpoch};
      if (r.civilBefore != r.civilAfter) return r;
      before = absl::FromUnixSeconds(r.at);
    }
    return std::nullopt;
  }
};

} // namespace datetime_detail

namespace dd = datetime_detail;

IanaZone::IanaZone(std::string name, std::unique_ptr<const dd::IanaZoneRules> rules)
  : zoneName(std::move(name)), rules(std::move(rules)) {
  static std::atomic<uint64_t> ids = 0;
  id = ++ids;
}

IanaZone::~IanaZone() = default;

std::chrono::seconds IanaZone::offsetAt(int64_t instant) const {
  return std::chrono::seconds{rules->offsetAt(dd::floorDiv(instant, 1000))};
}

ZoneInterval IanaZone::intervalAt(int64_t instant) const {
  // Traversals probe one interval many times, then the one that follows it.
  // Remember this thread's last answer (by id: a freed zone's address can be
  // reused).
  thread_local uint64_t hintId = 0;
  thread_local ZoneInterval hint;
  if (hintId == id && instant >= hint.begin && instant < hint.end) return hint;
  int64_t t = dd::floorDiv(instant, 1000);
  int64_t offset = rules->offsetAt(t);
  int64_t begin = dd::kMinEpochMs;
  if (hintId == id && instant == hint.end) {
    begin = hint.end;
  } else if (auto previous = rules->previous(t, offset)) {
    begin = dd::clampMillis(previous->at);
  }
  auto next = rules->next(t, offset);
  ZoneInterval interval{begin, next ? dd::clampMillis(next->at) : dd::kMaxEpochMs + 1,
                        std::chrono::seconds{offset}};
  // Traversals advance by these bounds; an interval that misses its own
  // instant would stall them.
  if (instant < interval.begin || instant >= interval.end) throw inconsistent(instant);
  hintId = id;
  hint = interval;
  return interval;
}

std::runtime_error IanaZone::inconsistent(int64_t millis) const {
  return std::runtime_error("time-zone rules for " + zoneName + " are inconsistent near "
                            + std::to_string(millis));
}

LocalOffsets IanaZone::localAt(int64_t civilMillis) const {
  // Transitions fall on whole seconds, so a civil second's classification
  // holds for each of its milliseconds.
  int64_t civil = dd::floorDiv(civilMillis, 1000);
  auto info = rules->tz.At(dd::kCivilEpoch + civil);
  int64_t before = civil - absl::ToUnixSeconds(info.pre);
  int64_t after = civil - absl::ToUnixSeconds(info.post);
  LocalOffsets local;
  if (info.kind != absl::TimeZone::TimeInfo::UNIQUE) {
    int64_t transition = absl::ToUnixSeconds(info.trans);
    bool skipped = info.kind == absl::TimeZone::TimeInfo::SKIPPED;
    local = {skipped ? LocalOffsets::Kind::SKIPPED : LocalOffsets::Kind::REPEATED,
             std::chrono::seconds{before}, std::chrono::seconds{after},
             dd::clampMillis((__int128)transition + (skipped ? before : after)),
             dd::clampMillis((__int128)transition + (skipped ? after : before))};
  } else {
    // A unique civil time stays unique until the gap or fold of the next
    // transition. The span starts at this second: resolvers walk forward.
    auto next = rules->next(civil - before, before);
    local = {LocalOffsets::Kind::UNIQUE, std::chrono::seconds{before}, std::chrono::seconds{before},
             dd::clampMillis((__int128)civil),
             next ? dd::clampMillis(std::min(next->civilBefore, next->civilAfter))
                  : dd::kMaxEpochMs + 1};
  }
  // Within the supported range; resolvers cache by this span.
  if (civilMillis >= dd::kMinEpochMs && civilMillis <= dd::kMaxEpochMs
      && (civilMillis < local.civilBegin || civilMillis >= local.civilEnd)) {
    throw inconsistent(civilMillis);
  }
  return local;
}

namespace datetime_detail {

ZoneRegistry ZoneRegistry::load(const std::string& directory) {
  ZoneRegistry registry;
  // cctz resolves even "file:" names against its own zoneinfo directory
  // unless they are absolute.
  std::error_code ec;
  std::string root = std::filesystem::absolute(directory, ec).string();
  if (ec) root = directory;
  std::ifstream in(root + "/tzdata.zi");
  if (!in) {
    registry.error = "cannot read " + root + "/tzdata.zi";
    return registry;
  }
  registry.version = "unknown";
  // Only the names come from tzdata.zi; the rules come from the TZif files
  // compiled from the same data.
  std::vector<std::string> zoneNames;
  std::vector<std::pair<std::string, std::string>> links;  // name, target
  std::string line;
  while (std::getline(in, line)) {
    std::string_view text = line;
    if (text.starts_with("# version ")) {
      registry.version = std::string(text.substr(10));
      continue;
    }
    auto field = [&](size_t index) {
      size_t begin = 0;
      for (;;) {
        begin = text.find_first_not_of(" \t", begin);
        if (begin == std::string_view::npos) return std::string_view{};
        size_t end = std::min(text.find_first_of(" \t", begin), text.size());
        if (index-- == 0) return text.substr(begin, end - begin);
        begin = end;
      }
    };
    std::string_view kind = field(0);
    if (kind == "Z") zoneNames.emplace_back(field(1));
    else if (kind == "L") links.emplace_back(field(2), field(1));
  }

  for (auto& name : zoneNames) {
    // tzdata.zi is trusted system data; still never form a path outside root.
    if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) continue;
    absl::TimeZone tz;
    if (!absl::LoadTimeZone("file:" + root + "/" + name, &tz)) continue;
    registry.zones.push_back(std::make_unique<IanaZone>(
        name, std::make_unique<const IanaZoneRules>(tz)));
    registry.names.emplace_back(name, registry.zones.back().get());
  }
  std::sort(registry.names.begin(), registry.names.end());
  // A link's own TZif file may be absent (e.g. tzdata-legacy); it resolves
  // through tzdata.zi to its target. Links to links resolve transitively.
  std::sort(links.begin(), links.end());
  auto target = [&](std::string_view name) -> std::string_view {
    auto it = std::lower_bound(links.begin(), links.end(), name,
        [](const auto& link, std::string_view key) { return link.first < key; });
    return it != links.end() && it->first == name ? std::string_view(it->second) : std::string_view{};
  };
  size_t zoneEntries = registry.names.size();
  for (auto& [name, linked] : links) {
    std::string_view resolved = linked;
    // A chain longer than the link count is a cycle.
    for (size_t depth = 0; depth < links.size() && !target(resolved).empty(); depth++) {
      resolved = target(resolved);
    }
    auto end = registry.names.begin() + (ptrdiff_t)zoneEntries;
    auto it = std::lower_bound(registry.names.begin(), end, resolved,
        [](const auto& entry, std::string_view key) { return entry.first < key; });
    if (it == end || it->first != resolved) continue;
    const IanaZone* zone = it->second;  // emplace_back may reallocate names
    registry.names.emplace_back(name, zone);
  }
  std::sort(registry.names.begin(), registry.names.end());
  registry.names.erase(std::unique(registry.names.begin(), registry.names.end(),
      [](const auto& a, const auto& b) { return a.first == b.first; }), registry.names.end());
  if (registry.zones.empty()) registry.error = "no loadable TZif zone files under " + root;
  return registry;
}

const IanaZone* ZoneRegistry::find(std::string_view name) const {
  auto it = std::lower_bound(names.begin(), names.end(), name,
      [](const auto& entry, std::string_view key) { return entry.first < key; });
  return it != names.end() && it->first == name ? it->second : nullptr;
}

const ZoneRegistry& zoneRegistry() {
  static const ZoneRegistry registry = [] {
    const char* dir = std::getenv("TZDIR");
    std::string root = dir != nullptr && *dir != '\0' ? dir : "/usr/share/zoneinfo";
    auto loaded = ZoneRegistry::load(root);
    if (loaded.error.empty()) {
      LOG_INFO("Loaded time-zone database version {} ({} zones from {})",
               loaded.version, loaded.zoneCount(), root);
    } else {
      LOG_ERROR("Time-zone database unavailable ({}). UTC and fixed offsets remain available",
                loaded.error);
    }
    return loaded;
  }();
  return registry;
}

} // namespace datetime_detail
} // namespace luxir
