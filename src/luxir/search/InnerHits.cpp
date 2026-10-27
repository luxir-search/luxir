// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/search/InnerHits.h"
#include "luxir/search/RelationReader.h"
#include "luxir/api/build.h"
#include <set>

namespace luxir {
void emitInnerHits(IndexReader& reader, std::span<Query::SegmentSource* const> sources,
    std::span<const segdoc> docs, api::DocList& output, std::pmr::memory_resource& mr) {
  if (sources.empty() || docs.empty()) return;
  struct Entry { api::InnerHits spec; std::string_view relation; std::span<const Query::ChildMatch> hits; int64_t count; };
  std::vector<std::vector<Entry>> entries(docs.size());
  bool any = false;
  for (size_t i = 0; i < docs.size(); ++i) {
    std::set<std::string_view> names;
    for (auto* source : sources) if (source) source->visitChildMatches(docs[i].segment(), docs[i].docId(),
      [&](const api::InnerHits& spec, std::string_view relation, std::span<const Query::ChildMatch> hits, int64_t count) {
        if (!names.insert(spec.name).second) throw RequestError("duplicate inner_hits name: " + std::string(spec.name));
        entries[i].push_back({spec, relation, hits, count}); any = true;
      });
  }
  if (!any) return;
  auto* rows = api::build::allocArray(output.matches, docs.size(), mr);
  for (size_t i = 0; i < docs.size(); ++i) for (const auto& entry : entries[i]) {
    auto& value = api::build::mapSlot(rows[i].fields, entries[i].size(), api::build::arenaStr(mr, entry.spec.name), mr)->kind.emplace<api::DocList>();
    value.found = entry.count;
    value.row_count = (int32_t)entry.hits.size();
    auto* hits = api::build::allocArray(value.docs, entry.hits.size(), mr);
    for (size_t h = 0; h < entry.hits.size(); ++h) {
      auto hit = entry.hits[h];
      auto json = RelationReader::strings(reader, hit.segment, "luxir_source_s", std::span(&hit.doc, 1));
      api::Map source;
      if (json[0].empty() || !api::read_json(source, api::build::arenaStr(mr, json[0]), mr)) throw std::runtime_error("invalid stored child source");
      size_t capacity = source.fields.size() + 2;
      for (auto [name, val] : source.fields) {
        if (!entry.spec.fields.empty() && name != "id" && !std::ranges::contains(entry.spec.fields, std::string_view("*"))
            && !std::ranges::contains(entry.spec.fields, name)) continue;
        *api::build::mapSlot(hits[h].fields, capacity, name, mr) = *val;
      }
      api::build::mapSlot(hits[h].fields, capacity, "_score_", mr)->kind = hit.score;
      api::build::mapSlot(hits[h].fields, capacity, "_relation_", mr)->kind = api::build::arenaStr(mr, entry.relation);
    }
  }
}
}
