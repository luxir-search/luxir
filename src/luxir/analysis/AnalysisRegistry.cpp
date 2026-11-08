// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/analysis/AnalysisRegistry.h"

#include <memory_resource>
#include <stdexcept>

namespace luxir {

// Registry-owned copy of an AnalyzerDef: the def's views point into `json` and
// `arena`, both of which live as long as the entry.
struct AnalysisRegistry::Preset {
  std::string json;
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
};

AnalysisRegistry::AnalysisRegistry() { registerBuiltins(); }
AnalysisRegistry::~AnalysisRegistry() = default;

AnalysisRegistry& AnalysisRegistry::global() {
  static AnalysisRegistry instance;
  return instance;
}

const AnalysisRegistry& globalAnalysisRegistry() { return AnalysisRegistry::global(); }

namespace {

template <class Map>
std::vector<std::string> keysOf(const Map& map) {
  std::vector<std::string> out;
  out.reserve(map.size());
  for (const auto& entry : map) out.push_back(entry.first);  // std::map: already sorted
  return out;
}

std::string joined(const std::vector<std::string>& names) {
  if (names.empty()) return "none";
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

void checkName(std::string_view kind, const std::string& name) {
  if (name.empty()) throw std::invalid_argument(std::string(kind) + " name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      throw std::invalid_argument("invalid " + std::string(kind) + " name '" + name + "'; use [a-z0-9_]+");
    }
  }
}

template <class Map, class Maker>
void addTo(Map& map, bool frozen, std::string_view kind, std::string name, Maker maker) {
  if (frozen) throw std::logic_error("analysis registry is frozen; components must be added during startup");
  checkName(kind, name);
  if (!maker) throw std::invalid_argument(std::string(kind) + " '" + name + "' has no maker");
  if (map.contains(name)) throw std::invalid_argument("duplicate " + std::string(kind) + " '" + name + "'");
  map.emplace(std::move(name), std::move(maker));
}

template <class Map>
const auto& findMaker(const Map& map, const api::AnalyzerComponent& def, std::string_view kind,
                      std::string_view plural) {
  if (def.name.empty()) {
    throw std::invalid_argument(std::string(kind) + " component has no name; valid " + std::string(plural) + ": " +
                                joined(keysOf(map)));
  }
  auto it = map.find(def.name);
  if (it == map.end()) {
    throw std::invalid_argument("unknown " + std::string(kind) + " '" + std::string(def.name) + "'; valid " +
                                std::string(plural) + ": " + joined(keysOf(map)));
  }
  return it->second;
}

}  // namespace

void AnalysisRegistry::addTokenizer(std::string name, TokenizerMaker maker) {
  addTo(tokenizers, isFrozen, "tokenizer", std::move(name), std::move(maker));
}
void AnalysisRegistry::addFilter(std::string name, FilterMaker maker) {
  addTo(filters, isFrozen, "filter", std::move(name), std::move(maker));
}
void AnalysisRegistry::addCharFilter(std::string name, CharFilterMaker maker) {
  addTo(charFilters, isFrozen, "char filter", std::move(name), std::move(maker));
}

void AnalysisRegistry::addAnalyzer(std::string name, const api::AnalyzerDef& def) {
  if (isFrozen) throw std::logic_error("analysis registry is frozen; components must be added during startup");
  checkName("analyzer", name);
  if (analyzers.contains(name)) throw std::invalid_argument("duplicate analyzer '" + name + "'");
  if (!def.name.empty()) {
    throw std::invalid_argument("analyzer '" + name + "' must be a complete definition, not the named analyzer '" +
                                std::string(def.name) + "'");
  }
  auto preset = std::make_unique<Preset>();
  if (!api::write_json(def, preset->json)) throw std::runtime_error("Cannot copy analyzer '" + name + "'");
  std::string err;
  if (!api::read_json(preset->def, preset->json, preset->arena, &err)) {
    throw std::runtime_error("Cannot copy analyzer '" + name + "': " + err);
  }
  analyzers.emplace(std::move(name), std::move(preset));
}

std::unique_ptr<const TokenizerFactory> AnalysisRegistry::makeTokenizer(const api::AnalyzerComponent& def) const {
  return findMaker(tokenizers, def, "tokenizer", "tokenizers")(def);
}
std::unique_ptr<TokenFilterFactory> AnalysisRegistry::makeFilter(const api::AnalyzerComponent& def) const {
  return findMaker(filters, def, "filter", "filters")(def);
}
std::unique_ptr<const CharFilterFactory> AnalysisRegistry::makeCharFilter(const api::AnalyzerComponent& def) const {
  return findMaker(charFilters, def, "char filter", "char filters")(def);
}

const api::AnalyzerDef* AnalysisRegistry::findAnalyzer(std::string_view name) const {
  auto it = analyzers.find(name);
  return it == analyzers.end() ? nullptr : &it->second->def;
}

std::vector<std::string> AnalysisRegistry::tokenizerNames() const { return keysOf(tokenizers); }
std::vector<std::string> AnalysisRegistry::filterNames() const { return keysOf(filters); }
std::vector<std::string> AnalysisRegistry::charFilterNames() const { return keysOf(charFilters); }
std::vector<std::string> AnalysisRegistry::analyzerNames() const { return keysOf(analyzers); }

void AnalysisRegistry::freeze() {
  if (isFrozen) return;
  // A named analyzer whose components do not resolve (a plugin forgot to
  // register one) fails startup here rather than at the first schema using it.
  for (const auto& [name, preset] : analyzers) {
    try {
      const api::AnalyzerDef& def = preset->def;
      if (def.tokenizer) makeTokenizer(*def.tokenizer);
      for (const auto& c : def.char_filters) makeCharFilter(c);
      for (const auto& f : def.filters) makeFilter(f);
    } catch (const std::invalid_argument& e) {
      throw std::invalid_argument("analyzer '" + name + "': " + e.what());
    }
  }
  isFrozen = true;
}

}  // namespace luxir
