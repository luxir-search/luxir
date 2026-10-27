// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/search/RelationReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/schema/Schema.h"
#include <numeric>

namespace luxir {
std::vector<std::string> RelationReader::strings(IndexReader& reader, int32_t segment,
    std::string_view field, std::span<const int32_t> docs) {
  std::vector<std::string> values(docs.size());
  auto* info = reader.field(field).at(segment);
  if (!info || docs.empty()) return values;
  auto guard = MemPool::threadLocalPoolGuard();
  auto& pool = guard.pool();
  auto& postings = reader.segments()[segment].postingsReader();
  if (info->flags & FieldType::INDEX_DOCS) {
    TermsEnum terms(pool, postings, *info);
    OrdColReader::getSingleValues(pool, postings, *info, docs,
      [&](size_t i, int32_t, int32_t ordinal) {
        terms.seekOrd(ordinal - 1);
        values[i] = (std::string_view)terms.term();
      });
  } else {
    StrColReader::getValues(pool, postings, *info, docs,
      [&](size_t i, int32_t, std::string_view value) { values[i] = value; });
  }
  return values;
}
RelationReader::RelationReader(IndexReader& reader) {
  std::map<std::string, Parent, std::less<>> families;
  std::vector<std::vector<std::string>> tokens;
  segments.resize(reader.segments().size());
  for (auto& segment : reader.segments()) {
    std::vector<int32_t> docs(segment.maxDoc());
    std::iota(docs.begin(), docs.end(), 0);
    auto& out = segments[segment.ord];
    out.parents.resize(docs.size());
    auto names = strings(reader, segment.ord, "luxir_relation_s", docs);
    out.relation.resize(docs.size());
    for (size_t i = 0; i < names.size(); ++i) if (!names[i].empty()) {
      auto [entry, inserted] = relationOrdinals.try_emplace(names[i], (uint32_t)relationOrdinals.size() + 1);
      out.relation[i] = entry->second;
    }
    tokens.push_back(strings(reader, segment.ord, "luxir_family_s", docs));
    DocSetBuilder roots(segment.maxDoc());
    for (auto doc : docs) {
      if (segment.liveDocs() && !segment.liveDocs()->docset().get(doc)) continue;
      if (out.relation[doc] != 0) continue;
      roots.add(doc);
      if (!tokens.back()[doc].empty()) families[tokens.back()[doc]] = {segment.ord, doc};
    }
    out.roots = roots.build();
  }
  for (auto& segment : reader.segments()) {
    auto& out = segments[segment.ord];
    for (int32_t doc = 0; doc < segment.maxDoc(); ++doc) {
      if (out.relation[doc] == 0 || (segment.liveDocs() && !segment.liveDocs()->docset().get(doc))) continue;
      auto found = families.find(tokens[segment.ord][doc]);
      if (found != families.end()) {
        out.parents[doc] = found->second;
        out.children.push_back(doc);
      }
    }
  }
}
RelationReader& IndexReader::relations() {
  std::lock_guard lock(relationsMutex);
  if (!relationsReader) relationsReader = std::make_shared<RelationReader>(*this);
  return *relationsReader;
}
DocSet* IndexReader::rootDomain(int32_t segment) {
  if (!schema()->relations.empty() || field("luxir_relation_s").exists()) return relations().segments[segment].roots.get();
  auto& seg = segments()[segment];
  return seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr;
}
}
