// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TermReads.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include "luxir/analysis/Analyzer.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/SummaryView.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

namespace {

// Walks the term-level queries of a request. With `segments` it collects the
// segments whose base a term lookup opens; with `probes` the term membership
// ranges the lookups will read.
class TermFields {
  const Schema& schema;
  IndexReader& reader;
  SegmentMask* segments;
  ReadSet* probes;

  // The dictionary terms a value seeks, or nullopt when they are not known here.
  static std::optional<std::vector<std::string>> termsOf(FieldType& type, std::optional<std::string_view> value) {
    if (!value) return std::nullopt;
    if (type.type() != FieldType::TEXT) return std::vector<std::string>{std::string(*value)};
    auto& text = static_cast<TextFieldType&>(type);
    if (!text.analyzer_) return std::nullopt;
    auto chain = text.analyzer_->createChain();
    Token& token = chain->getToken();
    std::vector<std::string> terms;
    PackedTerm::TermBuffer scratch;
    chain->setValue(*value);
    chain->reset();
    while (chain->tail->incrementToken()) {
      if (auto fitted = PackedTerm::fitTerm(type.longTerms, token.text, scratch)) terms.emplace_back(*fitted);
    }
    return terms;
  }

  void field(std::string_view name, std::optional<std::string_view> value) {
    auto* type = schema.getFieldTypePtr(name);
    if (!type) return;
    auto kind = type->type();
    if (kind != FieldType::TEXT && kind != FieldType::STRING && kind != FieldType::ID) return;
    auto present = reader.field(name);
    if (!present.exists()) return;
    auto terms = termsOf(*type, value);
    const auto& segs = reader.segments();
    for (size_t i = 0; i < segs.size(); i++) {
      if (!present.presentIn(i) || segs[i].isOpen()) continue;
      if (probes != nullptr) {
        if (!terms || !segs[i].hasTermMembership()) continue;
        for (const auto& term : *terms) {
          if (!SummaryView::provesTermAbsent(segs[i].summary(), name, term)) segs[i].declareMembershipProbes(name, term, *probes);
        }
      } else if (!segments->test(i)
                 && !(terms && std::ranges::all_of(*terms, [&](const auto& term) { return QueryPruning::termAbsent(segs[i], name, term); }))) {
        segments->set(i);
      }
    }
  }

  static std::optional<std::string_view> text(const api::Val* value) {
    if (!value) return std::nullopt;
    if (auto* string = std::get_if<std::string_view>(&value->kind)) return *string;
    return std::nullopt;
  }

  void queries(std::span<const api::Query> list) {
    for (const auto& q : list) query(q);
  }

public:
  TermFields(const Schema& schema, IndexReader& reader, SegmentMask* segments, ReadSet* probes)
      : schema(schema), reader(reader), segments(segments), probes(probes) {}

  template <class View>
  void query(const View& view) {
    if (view) query(*view);
  }

  void query(const api::Query& q) {
    std::visit([&](const auto& kind) { visit(kind); }, q.kind);
  }

  void visit(const api::Match& q) {
    auto value = text(q.val ? &*q.val : nullptr);
    // A non-string value seeks its coerced form: declare the field.
    field(q.field, value);
  }
  void visit(const api::AnyOfQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::PhraseQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::PrefixQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::FuzzyQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::WildcardQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::RegexQuery& q) { field(q.field, std::nullopt); }
  void visit(const api::MultiMatchQuery& q) {
    for (const auto& weighted : q.fields) field(weighted.field, std::nullopt);
  }
  void visit(const api::BooleanQuery& q) {
    queries(q.filter);
    queries(q.required);
    queries(q.optional);
    queries(q.prohibited);
  }
  void visit(const api::ConstantScoreQuery& q) { query(q.query); }
  void visit(const api::BoostQuery& q) { query(q.query); }
  void visit(const api::RescoreQuery& q) { query(q.query); }
  void visit(const api::DisMaxQuery& q) { queries(q.queries); }
  template <class Other>
  void visit(const Other&) {}  // no term dictionary seek, or not resolved here

  template <class Ops>
  void ops(const Ops& map) {
    for (const auto& entry : map) op(*entry.second);
  }

  void op(const api::SearchOp& op) {
    if (op.domain) {
      query(op.domain->query);
      queries(op.domain->filter);
    }
    if (auto* top = std::get_if<api::TopDocs>(&op.kind)) {
      query(top->query);
      for (const auto& filter : top->filter) query(filter.query);
      ops(top->ops);
    } else if (auto* facet = std::get_if<api::QueryFacet>(&op.kind)) {
      for (const auto& bucket : facet->buckets) query(bucket.query);
      ops(facet->ops);
    }
  }
};

}  // namespace

void declareTermReads(const api::SearchRequest& request, const Schema& schema, IndexReader& reader,
                      ReadSet& reads) {
  if (reader.segments().empty()) return;
  SegmentMask segments(reader.segments().size(), false);
  TermFields(schema, reader, &segments, nullptr).ops(request.ops);
  if (segments.popcount() > 0) reader.declareBases(segments, reads);
}

void declareTermProbes(const api::SearchRequest& request, const Schema& schema, IndexReader& reader, ReadSet& reads) {
  if (reader.segments().empty()) return;
  TermFields(schema, reader, nullptr, &reads).ops(request.ops);
}

}  // namespace luxir
