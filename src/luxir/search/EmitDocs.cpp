// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "EmitDocs.h"
#include <set>

namespace luxir {
namespace {

constexpr int32_t RETRIEVAL_BATCH_ROWS = 256;

api::DocList mappedDocuments(SearchResponse& response, MappedReturnFields& selection,
                             std::span<const segdoc> docs, uint64_t flags, RequestMemResource& mr) {
  api::DocList result;
  result.row_count = (int32_t)docs.size();
  DocBatchScratch scratch;
  loadDocuments(response, selection.target, docs, selection.fields, result, mr,
      {flags, false, api::DocFormat::ROWS, selection.fields.size() + 1, 0, false, &mr},
      [](int64_t) { return 0.0f; }, scratch);
  if (result.docs.empty()) build::allocArray(result.docs, docs.size(), mr);
  return result;
}

void mergeFields(std::vector<ReturnField>& fields, std::span<const ReturnField> additions) {
  for (const auto& field : additions) {
    auto found = std::ranges::find(fields, field.outputKey, &ReturnField::outputKey);
    if (found == fields.end()) fields.push_back(field);
    else if (field.kind == ReturnField::Kind::MAPPED && found->kind == field.kind) {
      mergeFields(found->source.mapped->fields, field.source.mapped->fields);
    }
  }
}

[[noreturn]] void selectorError(std::string_view selector, std::string_view message) {
  throw RequestError("Returned-field selector '" + std::string(selector) + "': " + std::string(message));
}

} // namespace

void loadMappedFields(SearchResponse& response, const ReturnField& field,
                      std::span<const segdoc> docs, std::span<RowIndex> rows,
                      api::DocList& output, uint64_t flags, RequestMemResource& mr) {
  auto& selection = *field.source.mapped;
  int32_t leaf = docs[rows.front()].segment();
  auto* shape = selection.target.segments()[leaf].segmentCore().ownership(selection.target.path());
  if (!shape) return;
  auto& column = std::get<api::ColVal>(build::columnSlot(output.columns, 0, field.outputKey, mr).kind);
  auto* target = const_cast<api::Val*>(column.v.data());
  RequestMemResource scratch(response.req.memoryTracker, "mapped returned rows", "returned fields");
  std::pmr::vector<segdoc> mapped(&scratch);
  std::pmr::vector<api::Val*> destinations(&scratch);
  mapped.reserve(RETRIEVAL_BATCH_ROWS);
  destinations.reserve(RETRIEVAL_BATCH_ROWS);
  auto flush = [&] {
    if (mapped.empty()) return;
    auto loaded = mappedDocuments(response, selection, mapped, flags, mr);
    for (size_t i = 0; i < mapped.size(); i++) destinations[i]->kind = loaded.docs[i];
    mapped.clear();
    destinations.clear();
  };
  for (auto row : rows) {
    auto [first, end] = shape->range(docs[row].docId());
    if (first == end) continue;
    auto& array = target[row].kind.emplace<api::ArrVal>();
    auto* values = build::allocArray(array.v, (size_t)(end - first), mr);
    for (int32_t child = first; child < end; child++) {
      mapped.emplace_back(leaf, child);
      destinations.push_back(&values[child - first]);
      if (mapped.size() == RETRIEVAL_BATCH_ROWS) flush();
    }
  }
  flush();
}

bool appendMappedSelector(SearchRequest& req, IndexReader& reader, std::string_view selector,
                          std::vector<ReturnField>& picked) {
  ArenaResource mr(&req.arena);
  std::string_view name = selector, path = reader.path();
  if (name.starts_with('/') || name.starts_with("..")) {
    std::string_view prefix;
    if (name.starts_with('/')) { path = {}; name.remove_prefix(1); }
    else {
      while (name.starts_with("../")) {
        if (path.empty()) selectorError(selector, "cannot ascend above the root view");
        path = spaceParentPath(path);
        name.remove_prefix(3);
      }
    }
    prefix = selector.substr(0, selector.size() - name.size());
    if (prefix.empty() || name.empty() || name.find('/') != std::string_view::npos) {
      selectorError(selector, "expected '../name', '../../name', or '/name' (or '*' in place of name)");
    }
    if (name.find('.') != std::string_view::npos) selectorError(selector, "up then down is not supported; select the ancestor's own fields");
    if (name == "_score_") selectorError(selector, "an ancestor has no score in this result");
    auto& target = *req.pinView(path);
    std::vector<ReturnField> inner;
    std::string_view selectors[]{name};
    appendReturnFields(req, target, selectors, inner);
    AncestorRows* mapping = nullptr;
    for (const auto& field : picked) {
      if (field.rows && &field.rows->reader == &target) mapping = field.rows;
    }
    for (auto field : inner) {
      if (field.kind == ReturnField::Kind::MAPPED) selectorError(selector, "up then down is not supported; select the ancestor's own fields");
      field.outputKey = build::arenaStr(mr, std::string(prefix) + std::string(field.outputKey));
      if (std::ranges::find(picked, field.outputKey, &ReturnField::outputKey) != picked.end()) continue;
      if (&reader == &target) { picked.push_back(field); continue; }
      if (!mapping) mapping = arenaCreate<AncestorRows>(req.arena, reader, target);
      field.rows = mapping;
      picked.push_back(field);
    }
    return true;
  }
  auto dot = name.find('.');
  auto first = name.substr(0, dot);
  // A representation selector can only occur on the final field component.
  if (dot != std::string_view::npos && first.find("__") != std::string_view::npos) return false;
  MappedReturnFields* mapping = nullptr;
  for (const auto& field : picked) {
    if (field.outputKey == first && field.kind == ReturnField::Kind::MAPPED) mapping = field.source.mapped;
  }
  IndexReader* destination = mapping ? &mapping->target : nullptr;
  if (!destination) {
    FieldResolver resolver(*req.schema, req.resolvedFields, reader.path());
    const auto& nested = resolver.resolvePath(first);
    destination = req.pinView(nested.physicalName).get();
  }
  auto& child = *destination;
  if (!mapping) {
    mapping = arenaCreate<MappedReturnFields>(req.arena, child);
    ReturnField outer{build::arenaStr(mr, first), {}, nullptr, ReturnField::Kind::MAPPED, false};
    outer.source.mapped = mapping;
    picked.push_back(outer);
  }
  if (dot != std::string_view::npos) {
    auto inner = name.substr(dot + 1);
    if (inner == "_score_") selectorError(selector, "a child row has no score in this result");
    std::string_view selectors[]{inner};
    std::vector<ReturnField> additions;
    appendReturnFields(req, child, selectors, additions);
    mergeFields(mapping->fields, additions);
  } else {
    std::string_view own[]{"*"};
    std::vector<ReturnField> additions;
    appendReturnFields(req, child, own, additions);
    mergeFields(mapping->fields, additions);
    std::set<std::string_view> paths;
    for (const auto& leaf : child.segments()) {
      // The manifest's space catalog, so listing sub-paths opens nothing.
      for (const auto& [path, rows] : leaf.basePart()->spaces) {
        if (spaceParentPath(path) == child.path()) paths.insert(path);
      }
    }
    for (auto path : paths) {
      std::string_view local[]{path.substr(child.path().size() + 1)};
      additions.clear();
      appendReturnFields(req, child, local, additions);
      mergeFields(mapping->fields, additions);
    }
  }
  return true;
}

} // namespace luxir
