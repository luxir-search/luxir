// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/analysis/Analyzer.h"
#include "luxir/api/luxir_types.hpp"

namespace luxir {

// The analysis component registry: name -> maker for tokenizers, token filters
// and char filters, plus named analyzers (presets). The process-wide instance is
// AnalysisRegistry::global(). Construction seeds built-ins without initializing
// or freezing plugins. initPlugins() adds plugin components and freezes it.
// Concurrent reads are safe even while unfrozen, provided no thread registers
// components or freezes the registry during reads. Makers must support
// concurrent calls; built-in makers create independent factories.
//
// A maker receives the authored component (name + typed params), validates the
// params, and returns an immutable factory that creates one stage per chain.
// Makers throw std::invalid_argument for bad params; the message should teach.
class AnalysisRegistry {
public:
  using TokenizerMaker = std::function<std::unique_ptr<const TokenizerFactory>(const api::AnalyzerComponent&)>;
  // Filter factories are mutable until Analyzer::compile has run their prepare().
  using FilterMaker = std::function<std::unique_ptr<TokenFilterFactory>(const api::AnalyzerComponent&)>;
  using CharFilterMaker = std::function<std::unique_ptr<const CharFilterFactory>(const api::AnalyzerComponent&)>;

private:
  struct Preset;
  std::map<std::string, TokenizerMaker, std::less<>> tokenizers;
  std::map<std::string, FilterMaker, std::less<>> filters;
  std::map<std::string, CharFilterMaker, std::less<>> charFilters;
  std::map<std::string, std::unique_ptr<Preset>, std::less<>> analyzers;
  bool isFrozen = false;

  void registerBuiltins();
  void registerBuiltinCharFilters();

public:
  static AnalysisRegistry& global();

  AnalysisRegistry();
  ~AnalysisRegistry();
  AnalysisRegistry(const AnalysisRegistry&) = delete;
  AnalysisRegistry& operator=(const AnalysisRegistry&) = delete;

  // Names are [a-z0-9_]+. A duplicate within a kind throws
  // std::invalid_argument; any add after freeze() throws std::logic_error.
  void addTokenizer(std::string name, TokenizerMaker maker);
  void addFilter(std::string name, FilterMaker maker);
  void addCharFilter(std::string name, CharFilterMaker maker);
  // A named analyzer: a complete AnalyzerDef the name expands to. The
  // definition may not itself be a named analyzer (no recursion). The
  // definition is deep-copied into registry-owned storage (the caller's
  // arena views may die right after the call); its components are validated
  // by freeze(), so they may be registered after the preset.
  void addAnalyzer(std::string name, const api::AnalyzerDef& def);

  // Throw std::invalid_argument with a teaching message ("unknown filter 'x';
  // valid filters: a, b, c"); names are listed sorted.
  std::unique_ptr<const TokenizerFactory> makeTokenizer(const api::AnalyzerComponent& def) const;
  std::unique_ptr<TokenFilterFactory> makeFilter(const api::AnalyzerComponent& def) const;
  std::unique_ptr<const CharFilterFactory> makeCharFilter(const api::AnalyzerComponent& def) const;
  // The registered definition for a named analyzer, or nullptr. Stable for the
  // registry's lifetime.
  const api::AnalyzerDef* findAnalyzer(std::string_view name) const;

  std::vector<std::string> tokenizerNames() const;
  std::vector<std::string> filterNames() const;
  std::vector<std::string> charFilterNames() const;
  std::vector<std::string> analyzerNames() const;

  // Validates that every named analyzer's components resolve (throws
  // std::invalid_argument naming the analyzer), then rejects further adds.
  void freeze();
  bool frozen() const { return isFrozen; }
};

// Read-only access to the area-owned global registry.
const AnalysisRegistry& globalAnalysisRegistry();

}  // namespace luxir
