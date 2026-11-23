// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "DeclaredReads.h"

#include <set>
#include <tuple>

#include "luxir/analysis/Analyzer.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/store/AcquireRound.h"
#include "luxir/util/IoContext.h"

namespace luxir {

namespace {

using Mode = DeclaredReads::Lookup::Mode;

// Walks the term-level queries of a request for the lookups parsing makes:
// match, phrase and multi_match terms (analyzed as the parser does), and the
// fields that prefix, fuzzy, pattern and any_of queries look up. Only
// physical field names are resolved; what the walk misses is read on demand.
class TermWalk {
  const Schema& schema;
  std::vector<std::tuple<std::string_view, Mode, std::string>>& out;

  void terms(std::string_view field, std::string_view text) {
    auto* type = schema.getFieldTypePtr(field);
    if (!type) return;
    if (type->type() == FieldType::TEXT) {
      auto& textType = static_cast<TextFieldType&>(*type);
      if (!textType.analyzer_) return lookup(field, Mode::ROUTE);
      auto chain = textType.analyzer_->createChain();
      Token& token = chain->getToken();
      PackedTerm::TermBuffer scratch;
      chain->setValue(text);
      chain->reset();
      while (chain->tail->incrementToken()) {
        if (auto fitted = PackedTerm::fitTerm(type->longTerms, token.text, scratch)) {
          out.emplace_back(field, Mode::TERM, std::string(*fitted));
        }
      }
    } else if (type->type() == FieldType::STRING || type->type() == FieldType::ID) {
      std::string term(text);
      if (auto* string = dynamic_cast<StrFieldType*>(type)) string->normalize(term);
      PackedTerm::TermBuffer scratch;
      if (auto fitted = PackedTerm::fitTerm(type->longTerms, term, scratch)) {
        out.emplace_back(field, Mode::TERM, std::string(*fitted));
      }
    }
  }

  void lookup(std::string_view field, Mode mode) {
    auto* type = schema.getFieldTypePtr(field);
    if (type && (type->type() == FieldType::TEXT || type->type() == FieldType::STRING
                 || type->type() == FieldType::ID)) {
      out.emplace_back(field, mode, std::string());
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

  template <class View>
  void query(const View& view) {
    if (view) query(*view);
  }

  void visit(const api::Match& q) {
    if (auto value = text(q.val ? &*q.val : nullptr)) terms(q.field, *value);
    else lookup(q.field, Mode::ROUTE);
  }
  void visit(const api::PhraseQuery& q) {
    if (!q.text.empty()) terms(q.field, q.text);
    for (auto word : q.words) terms(q.field, word);
  }
  void visit(const api::MultiMatchQuery& q) {
    for (const auto& weighted : q.fields) {
      if (!q.text.empty()) terms(weighted.field, q.text);
    }
  }
  void visit(const api::AnyOfQuery& q) { lookup(q.field, Mode::ROUTE); }
  void visit(const api::PrefixQuery& q) { lookup(q.field, Mode::SCAN); }
  void visit(const api::FuzzyQuery& q) { lookup(q.field, Mode::SCAN); }
  void visit(const api::WildcardQuery& q) { lookup(q.field, Mode::SCAN); }
  void visit(const api::RegexQuery& q) { lookup(q.field, Mode::SCAN); }
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
  void visit(const Other&) {}  // no dictionary lookup, or one the walk does not resolve

  template <class Ops>
  void ops(const Ops& map) {
    for (const auto& entry : map) op(*entry.second);
  }

public:
  TermWalk(const Schema& schema, std::vector<std::tuple<std::string_view, Mode, std::string>>& out)
      : schema(schema), out(out) {}

  void query(const api::Query& q) {
    std::visit([&](const auto& kind) { visit(kind); }, q.kind);
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

  void request(const api::SearchRequest& request) { ops(request.ops); }
};

}  // namespace

// The parts of one round, by segment, with their bytes counted against the cap.
struct DeclaredReads::Round {
  enum class Added { HELD, DECLARED, DROPPED };

  DeclaredReads& owner;
  std::vector<std::pair<Segment*, std::vector<SegmentCore::Part>>> segments;
  std::set<std::tuple<SegmentCore*, uint64_t, uint64_t>> seen;

  Added add(Segment& segment, field_parts::Slice part, ReadClass readClass) {
    if (!part) return Added::HELD;
    auto& core = segment.segmentCore();
    if (core.held(part)) return Added::HELD;
    if (!seen.emplace(&core, part.location.raw(), part.bytes).second) return Added::DECLARED;
    if (owner.maxBytes && owner.bytes + part.bytes > owner.maxBytes) {
      owner.dropped += part.bytes;
      return Added::DROPPED;
    }
    owner.bytes += part.bytes;
    if (segments.empty() || segments.back().first != &segment) {
      auto found = std::ranges::find(segments, &segment, &decltype(segments)::value_type::first);
      if (found == segments.end()) segments.emplace_back(&segment, std::vector<SegmentCore::Part>());
      else std::iter_swap(found, segments.end() - 1);
    }
    segments.back().second.push_back({part, readClass});
    return Added::DECLARED;
  }
};

DeclaredReads::DeclaredReads(IndexReader& reader, const Schema& schema, const api::SearchRequest& request,
                             uint64_t maxBytes)
    : reader(reader), maxBytes(maxBytes) {
  std::vector<std::tuple<std::string_view, Mode, std::string>> found;
  TermWalk(schema, found).request(request);
  auto segments = reader.segments();
  for (auto& [field, mode, term] : found) {
    auto present = reader.field(field);
    if (!present.exists()) continue;
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto* record = present.at(i);
      if (!record || !record->terms()) continue;
      lookups.push_back({&segments[i], record, mode, term});
    }
  }
}

DeclaredReads::~DeclaredReads() = default;

bool DeclaredReads::needed(IndexReader& reader) {
  return !std::ranges::all_of(reader.segments(), [](const Segment& segment) {
    return segment.segmentCore().holdsFree();
  });
}

// Declares the next part the lookup reads that the segment does not hold.
void DeclaredReads::advance(Lookup& lookup, Round& round) {
  auto& segment = *lookup.segment;
  if (!lookup.terms) {
    // Round 1: the routing data, and the dictionary when it is read whole.
    auto dictionary = lookup.record->terms()->dictionaryBlocks();
    bool whole = lookup.mode == Mode::SCAN
        || (lookup.mode == Mode::TERM && TermsEnum::readsWhole(segment.segmentCore(), dictionary));
    auto routing = round.add(segment, TermsEnum::navigationPart(*lookup.record), ReadClass::METADATA);
    auto blocks = whole ? round.add(segment, dictionary, ReadClass::DATA) : Round::Added::HELD;
    if (routing == Round::Added::DROPPED || blocks == Round::Added::DROPPED || lookup.mode != Mode::TERM) {
      lookup.done = true;
      return;
    }
    if (routing == Round::Added::DECLARED || blocks == Round::Added::DECLARED) return;
    lookup.terms = pool.make<TermsEnum>(pool, segment.postingsReader(), *lookup.record);
  }
  // Round 2: the block the term lands in.
  switch (round.add(segment, lookup.terms->seekPart(lookup.term), ReadClass::DATA)) {
    case Round::Added::DECLARED: return;
    case Round::Added::DROPPED: lookup.done = true; return;
    case Round::Added::HELD: break;
  }
  // Round 3: the postings of a term the segment holds.
  lookup.done = true;
  if (!lookup.terms->probe(lookup.term)) return;
  lookup.terms->postingsParts([&](field_parts::Slice part) { round.add(segment, part, ReadClass::DATA); });
}

void DeclaredReads::declarePlanned(const PlannedRead& read, Round& round) {
  auto segments = read.view->segments();
  for (size_t i = 0; i < segments.size(); ++i) {
    if (read.term && (i >= read.term->postingsStates.size() || !read.term->postingsStates[i])) continue;
    const auto* record = read.field->segInfos.at(i);
    if (!record) continue;
    switch (read.kind) {
      case PlannedRead::Kind::NORMS:
        if (!record->norms()) break;
        round.add(segments[i], record->normsValues(segments[i].maxDoc()), ReadClass::DATA);
        if (record->docsWithField != segments[i].maxDoc()) {
          round.add(segments[i], record->docsBitmap(), ReadClass::DATA);
        }
        break;
    }
  }
}

bool DeclaredReads::next(AcquireRound& acquire) {
  Round round{*this, {}, {}};
  std::vector<std::shared_ptr<SegmentLive>> live;
  if (!planned) {
    if (!liveDeclared) {
      // The live docs of every segment with deletes go with the first round.
      liveDeclared = true;
      for (auto& segment : reader.segments()) {
        auto& root = segment.rootLive();
        if (root && segment.hasDeletes()) live.push_back(root);
      }
    }
    // Routing data, blocks, postings: three rounds at most. A part a failed
    // batch left unheld is read on demand, not declared again and again.
    if (++termRounds <= 3) {
      for (auto& lookup : lookups) {
        if (!lookup.done) advance(lookup, round);
      }
    }
  } else {
    for (const auto& read : registered.take()) declarePlanned(read, round);
  }
  bool issued = false;
  for (auto& root : live) {
    if (root->acquireAhead(acquire)) {
      acquire.keep(root);
      issued = true;
    }
  }
  for (auto& [segment, parts] : round.segments) {
    segment->segmentCore().acquireParts(parts, acquire);
    acquire.keep(segment->segmentCoreShared());
    issued = true;
  }
  if (issued) countAcquireRound();
  return issued;
}

}  // namespace luxir
