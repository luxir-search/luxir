// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "luxir/search/EmitDocs.h"
#include "luxir/search/ops/ScopeOp.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/Overloaded.h"

namespace luxir {

// Per-row sub-operations (TopDocs.row_ops): for every returned row, a top_docs
// over that row's own nested elements, returned as the row's value under the
// operation's name. The domain is the row's live elements of the path. With
// domain.from_match it is further restricted to the elements the named
// to_parent clause of the list's query matches, and those elements are scored
// by that clause's element query, the same computation the clause ran for
// that row. A fresh query in the row operation is a new computation over the
// domain. Each row runs one NestedScopeRequest, kept until the list's request
// ends so the emitted values can reference it.
class RowOps {
  struct Op {
    std::string_view name;
    IndexReader* scope;
    api::SearchOp op;  // top_docs over the elements; domain is the row
  };
  SearchRequest& req;
  std::vector<Op> ops;
  std::mutex mutex;
  std::vector<NestedScopeRequest::Handle> runs;

  // The element query of the to_parent clause named `name` in `query`.
  static const api::ToParentQuery* findClause(const api::Query& query, std::string_view name) {
    const api::ToParentQuery* found = nullptr;
    auto visit = [&](auto&& self, const api::Query& q) -> void {
      if (found) return;
      auto child = [&](const auto& view) { if (view.has_value()) self(self, *view); };
      auto list = [&](std::span<const api::Query> qs) { for (const auto& c : qs) self(self, c); };
      std::visit(overloaded{
        [&](const api::ToParentQuery& t) { if (t.name == name) found = &t; },
        [&](const api::BooleanQuery& b) { list(b.filter); list(b.required); list(b.optional); list(b.prohibited); },
        [&](const api::ConstantScoreQuery& c) { child(c.query); },
        [&](const api::BoostQuery& b) { child(b.query); },
        [&](const api::RescoreQuery& r) { child(r.query); },
        [&](const api::DisMaxQuery& d) { list(d.queries); },
        [&](const auto&) {},
      }, q.kind);
    };
    visit(visit, query);
    return found;
  }

  static bool containsKnn(const api::Query& query) {
    bool found = false;
    auto visit = [&](auto&& self, const api::Query& q) -> void {
      if (found) return;
      auto child = [&](const auto& view) { if (view.has_value()) self(self, *view); };
      auto list = [&](std::span<const api::Query> qs) { for (const auto& c : qs) self(self, c); };
      std::visit(overloaded{
        [&](const api::KnnQuery&) { found = true; },
        [&](const api::BooleanQuery& b) { list(b.filter); list(b.required); list(b.optional); list(b.prohibited); },
        [&](const api::ConstantScoreQuery& c) { child(c.query); },
        [&](const api::BoostQuery& b) { child(b.query); },
        [&](const api::RescoreQuery& r) { child(r.query); },
        [&](const api::DisMaxQuery& d) { list(d.queries); },
        [&](const auto&) {},
      }, q.kind);
    };
    visit(visit, query);
    return found;
  }

public:
  RowOps(SearchRequest& req, const api::TopDocs& list) : req(req) {
    if (req.schema->isScope()) {
      throw RequestError("row_ops cannot be nested inside nested path '" + req.schema->scopePath() + "'");
    }
    for (const auto& [name, view] : list.row_ops) {
      const api::SearchOp& source = *view;
      std::string where = "row_ops '" + std::string(name) + "'";
      if (!std::holds_alternative<api::TopDocs>(source.kind)) {
        throw RequestError(where + " must be a top_docs over the row's elements");
      }
      const auto& rowList = std::get<api::TopDocs>(source.kind);
      if (list.collapse) throw RequestError(where + ": row_ops apply to uncollapsed lists");
      if (!rowList.row_ops.empty() || rowList.collapse || !rowList.ops.empty()) {
        throw RequestError(where + " supports query, filter, sort, limit, offset, fields and diversify");
      }
      if (!source.domain || (source.domain->to_children.empty() && source.domain->from_match.empty())) {
        throw RequestError(where + " needs domain.to_children (a nested path) or domain.from_match (a to_parent name)");
      }
      const auto& domain = *source.domain;
      if (domain.query || domain.apply_parent_filters || domain.to_parent) {
        throw RequestError(where + ".domain supports to_children, from_match and filter");
      }
      Op op{name, nullptr, source};
      std::string_view path = domain.to_children;
      const api::ToParentQuery* clause = nullptr;
      if (!domain.from_match.empty()) {
        clause = list.query.has_value() ? findClause(*list.query, domain.from_match) : nullptr;
        if (clause == nullptr) {
          throw RequestError(where + ".domain.from_match: no to_parent clause named '" +
                             std::string(domain.from_match) + "' in the list's query");
        }
        if (containsKnn(*clause->query)) {
          throw RequestError(where + ".domain.from_match: clause '" + std::string(domain.from_match) +
                             "' selects elements by kNN, whose admitted window is global; replaying it per row "
                             "would be a different search. Use domain.to_children with an explicit knn query");
        }
        if (!path.empty() && path != clause->path) {
          throw RequestError(where + ".domain: to_children and the from_match clause name different paths");
        }
        path = clause->path;
      }
      op.scope = req.reader->scope(path);
      if (op.scope == nullptr) throw RequestError(where + ": '" + std::string(path) + "' is not a nested field");
      auto& rowTop = std::get<api::TopDocs>(op.op.kind);
      op.op.domain.reset();
      if (!domain.filter.empty() || (clause && rowTop.query.has_value())) {
        auto& scoped = op.op.domain.emplace();
        // The clause restricts the domain when the row operation brings its
        // own query; otherwise it is the query and scores the elements.
        if (clause && rowTop.query.has_value()) {
          auto* filters = (api::Query*)req.arena.AllocateAligned(sizeof(api::Query) * (domain.filter.size() + 1),
                                                                   alignof(api::Query));
          for (size_t i = 0; i < domain.filter.size(); i++) new (&filters[i]) api::Query(domain.filter[i]);
          new (&filters[domain.filter.size()]) api::Query(*clause->query);
          scoped.filter = std::span<const api::Query>(filters, domain.filter.size() + 1);
        } else {
          scoped.filter = domain.filter;
        }
      }
      if (clause && !rowTop.query.has_value()) rowTop.query = clause->query;
      ops.push_back(std::move(op));
    }
  }

  // Row values, one extra column per row operation.
  std::vector<ExtraColumn> columns() {
    std::vector<ExtraColumn> out;
    for (auto& op : ops) {
      ExtraColumn column;
      column.name = op.name;
      column.value = [](int64_t, api::Val&, std::pmr::memory_resource&) { assert(false); };
      column.batchValue = [this, &op](std::span<const segdoc> segDocs, std::span<const uint32_t> rows,
                                      std::span<api::Val> values, std::pmr::memory_resource& mr) {
        unused(mr);
        auto segments = op.scope->segments();
        for (uint32_t row : rows) {
          const segdoc& sd = segDocs[row];
          auto& elements = segments[(size_t)sd.segment()];
          std::vector<std::shared_ptr<const void>> held;
          std::vector<DocSet*> base(segments.size());
          auto empty = std::make_shared<ArrDocSet>(std::vector<int32_t>{});
          for (auto& set : base) set = empty.get();
          held.push_back(empty);
          std::vector<int32_t> docs;
          if (elements.maxDoc() > 0) {
            auto [begin, end] = elements.postingsReader().owners().range(sd.docId());
            auto* live = elements.liveDocSet();
            for (int32_t r = begin; r < end; r++) {
              if (!live || live->get(r)) docs.push_back(r);
            }
          }
          auto rowSet = std::make_shared<ArrDocSet>(std::move(docs));
          base[(size_t)sd.segment()] = rowSet.get();
          held.push_back(rowSet);
          auto run = NestedScopeRequest::run(req, *op.scope, op.name, op.op, std::move(base), std::move(held));
          if (const auto* value = run->result(op.name)) values[row] = *value;
          std::lock_guard lock(mutex);
          runs.push_back(std::move(run));
        }
      };
      out.push_back(std::move(column));
    }
    return out;
  }
};

} // namespace luxir
