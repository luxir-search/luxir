// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Highlighter.h"

#include <algorithm>
#include <map>
#include <numeric>

#include "luxir/api/build.h"
#include "luxir/query/AutomatonQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryShape.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/util/proto.h"

namespace luxir {
namespace {

// Byte-wise Levenshtein distance (FuzzyQuery's metric), or maxEdits + 1 as
// soon as it must exceed maxEdits.
int boundedEditDistance(std::string_view a, std::string_view b, int maxEdits) {
  if ((int)std::max(a.size(), b.size()) - (int)std::min(a.size(), b.size()) > maxEdits) {
    return maxEdits + 1;
  }
  std::vector<int> row(b.size() + 1);
  std::iota(row.begin(), row.end(), 0);
  for (size_t i = 1; i <= a.size(); i++) {
    int diagonal = row[0];
    row[0] = (int)i;
    int best = row[0];
    for (size_t j = 1; j <= b.size(); j++) {
      int above = row[j];
      row[j] = std::min({above + 1, row[j - 1] + 1, diagonal + (a[i - 1] != b[j - 1] ? 1 : 0)});
      diagonal = above;
      best = std::min(best, row[j]);
    }
    if (best > maxEdits) return maxEdits + 1;
  }
  return row[b.size()];
}

using Targets = std::map<std::string_view, HighlightTarget>;

// Gather the leaves that can match text: every clause but MUST_NOT ones.
// Fuzzy, prefix and pattern leaves are re-matched against each analyzed
// token rather than expanded against the index.
void collectLeaves(const Query* query, Targets& out) {
  if (query == nullptr) return;
  auto target = [&](std::string_view field) -> HighlightTarget& {
    auto& entry = out[field];
    entry.physicalName = field;
    return entry;
  };
  switch (query->getKind()) {
    case QueryKind::TERM: {
      auto* term = static_cast<const TermQuery*>(query);
      target(term->getField()).terms.insert(term->getTerm());
      break;
    }
    case QueryKind::TERM_IN_SET: {
      auto* set = static_cast<const TermInSetQuery*>(query);
      auto& entry = target(set->getField());
      for (std::string_view term : set->getTerms()) entry.terms.insert(term);
      break;
    }
    case QueryKind::PHRASE: {
      auto* phrase = static_cast<const PhraseQuery*>(query);
      auto& entry = target(phrase->getField());
      // A sloppy phrase marks its terms wherever they occur.
      if (phrase->getSlop() == 0) entry.phrases.push_back(phrase);
      else for (std::string_view term : phrase->getTerms()) entry.terms.insert(term);
      break;
    }
    case QueryKind::PREFIX:
    case QueryKind::AUTOMATON:
      target(static_cast<const MultiTermQuery*>(query)->getField()).multiTerms.push_back(query);
      break;
    case QueryKind::FUZZY:
      target(static_cast<const FuzzyQuery*>(query)->getField()).multiTerms.push_back(query);
      break;
    case QueryKind::BOOLEAN: {
      auto* boolean = static_cast<const BooleanQuery*>(query);
      for (auto clauses : {boolean->getMandatory(), boolean->getOptional(), boolean->getFilter()}) {
        for (const Query* clause : clauses) collectLeaves(clause, out);
      }
      break;
    }
    case QueryKind::BOOST:
      collectLeaves(static_cast<const BoostQuery*>(query)->getChild(), out);
      break;
    case QueryKind::CONSTANT_SCORE:
      collectLeaves(static_cast<const ConstantScoreQuery*>(query)->getChild(), out);
      break;
    case QueryKind::FORCE_PREPARE:
      collectLeaves(static_cast<const ForcePrepareQuery*>(query)->getChild(), out);
      break;
    case QueryKind::RESCORE:
      collectLeaves(static_cast<const RescoreQuery*>(query)->getChild(), out);
      break;
    case QueryKind::MIN_SCORE:
      collectLeaves(static_cast<const MinScoreQuery*>(query)->getChild(), out);
      break;
    default:
      break;
  }
}

std::string_view rootOf(std::string_view physical) {
  auto split = physical.rfind("__");
  return split == std::string_view::npos ? physical : physical.substr(0, split);
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isContinuation(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

struct Span {
  size_t start;
  size_t end;
};

struct Fragment {
  size_t score;
  size_t value;
  size_t start;
  std::string text;
};

// Marks and fragments one highlighted field of one document. Owns the
// field's analysis chains, so one instance serves a task's rows.
class FieldHighlighter {
  struct Token {
    int64_t position;
    Span span;
    std::string text;  // fitted term bytes
  };
  struct Analysis {
    const HighlightTarget* target;
    std::unique_ptr<TokenChain> chain;  // TEXT targets only
  };

  const HighlightPlan& plan;
  std::vector<Analysis> analyses;
  std::vector<Token> tokens;
  std::vector<Span> spans;
  std::vector<Fragment> fragments;
  size_t values = 0;

public:
  FieldHighlighter(const HighlightPlan& plan, const HighlightField& field) : plan(plan) {
    for (const auto& target : field.targets) {
      auto& analysis = analyses.emplace_back(&target, nullptr);
      if (target.fieldType->type() == FieldType::TEXT) {
        analysis.chain = static_cast<TextFieldType*>(target.fieldType)->createAnalyzer(target.physicalName);
      }
    }
  }

  void addValue(std::string_view value) {
    size_t valueIndex = values++;
    markValue(value);
    if (spans.empty()) return;
    if (plan.numberOfFragments == 0) {
      fragments.push_back({spans.size(), valueIndex, 0, render(value, 0, value.size())});
      return;
    }
    // One candidate window per match not already covered, scored by the
    // matches it holds; keep this value's best non-overlapping windows.
    std::vector<Fragment> candidates;
    std::vector<Span> windows;
    size_t covered = 0;
    for (size_t anchor = 0; anchor < spans.size(); anchor++) {
      if (anchor > 0 && spans[anchor].end <= covered) continue;
      Span window = windowAt(value, anchor);
      covered = window.end;
      windows.push_back(window);
    }
    std::vector<size_t> order(windows.size());
    std::iota(order.begin(), order.end(), 0);
    auto score = [&](const Span& window) {
      auto first = std::ranges::lower_bound(spans, window.start, {}, &Span::start);
      auto last = std::ranges::lower_bound(spans, window.end, {}, &Span::start);
      return (size_t)(last - first);
    };
    std::ranges::stable_sort(order, std::greater<>(), [&](size_t i) { return score(windows[i]); });
    std::vector<Span> chosen;
    for (size_t i : order) {
      if (chosen.size() == (size_t)plan.numberOfFragments) break;
      const Span& window = windows[i];
      bool overlaps = std::ranges::any_of(chosen, [&](const Span& other) {
        return window.start < other.end && other.start < window.end;
      });
      if (overlaps) continue;
      chosen.push_back(window);
      fragments.push_back({score(window), valueIndex, window.start,
                           render(value, window.start, window.end)});
    }
  }

  // The document's fragments: the best number_of_fragments across its
  // values, in value and text order. Resets for the next document.
  std::span<const std::string_view> finish(std::pmr::memory_resource& mr) {
    if (plan.numberOfFragments > 0 && fragments.size() > (size_t)plan.numberOfFragments) {
      std::ranges::stable_sort(fragments, std::greater<>(), &Fragment::score);
      fragments.resize((size_t)plan.numberOfFragments);
    }
    std::ranges::sort(fragments, {}, [](const Fragment& f) { return std::pair(f.value, f.start); });
    std::span<const std::string_view> out;
    auto* texts = api::build::allocArray(out, fragments.size(), mr);
    for (size_t i = 0; i < fragments.size(); i++) texts[i] = api::build::arenaStr(mr, fragments[i].text);
    fragments.clear();
    values = 0;
    return out;
  }

private:
  // Fill `spans` with the merged source byte ranges of value's matches.
  void markValue(std::string_view value) {
    spans.clear();
    size_t limit = std::min(value.size(), (size_t)plan.maxAnalyzedOffset);
    while (limit < value.size() && limit > 0 && isContinuation(value[limit])) limit--;
    std::string_view analyzed = value.substr(0, limit);
    PackedTerm::TermBuffer scratch;
    for (auto& analysis : analyses) {
      const HighlightTarget& target = *analysis.target;
      FieldType& type = *target.fieldType;
      if (!analysis.chain) {
        // A whole-value representation: one token, normalized like the index.
        std::string term(value);
        if (type.type() == FieldType::STRING) static_cast<StrFieldType&>(type).normalize(term);
        auto fitted = PackedTerm::fitTerm(type.longTerms, term, scratch);
        if (fitted && target.matchesTerm(term, *fitted)) spans.push_back({0, value.size()});
        continue;
      }
      TokenChain& chain = *analysis.chain;
      ::luxir::Token& token = chain.head.getToken();
      chain.head.setValue(analyzed);
      chain.reset();
      tokens.clear();
      int64_t position = -1;
      while (chain.tail->incrementToken()) {
        position += token.positionIncrement;
        auto fitted = PackedTerm::fitTerm(type.longTerms, token.text, scratch);
        if (!fitted) continue;
        Span span{(size_t)token.startOffset, (size_t)token.endOffset};
        if (target.matchesTerm(token.text, *fitted)) spans.push_back(span);
        if (!target.phrases.empty()) tokens.push_back({position, span, std::string(*fitted)});
      }
      for (const PhraseQuery* phrase : target.phrases) markPhrase(*phrase);
    }
    std::ranges::sort(spans, {}, [](const Span& s) { return std::pair(s.start, s.end); });
    size_t merged = 0;
    for (const Span& span : spans) {
      if (merged > 0 && span.start < spans[merged - 1].end) {
        spans[merged - 1].end = std::max(spans[merged - 1].end, span.end);
      } else {
        spans[merged++] = span;
      }
    }
    spans.resize(merged);
  }

  // Mark every occurrence of an exact phrase: each term at its relative
  // position (positions are non-decreasing in token order).
  void markPhrase(const PhraseQuery& phrase) {
    auto terms = phrase.getTerms();
    auto positions = phrase.getPositions();
    std::vector<size_t> matched;
    for (size_t i = 0; i < tokens.size(); i++) {
      if (tokens[i].text != terms[0]) continue;
      int64_t base = tokens[i].position - positions[0];
      matched.assign(1, i);
      for (size_t k = 1; k < terms.size(); k++) {
        int64_t want = base + positions[k];
        auto it = std::ranges::lower_bound(tokens, want, {}, &Token::position);
        while (it != tokens.end() && it->position == want && it->text != terms[k]) ++it;
        if (it == tokens.end() || it->position != want) break;
        matched.push_back((size_t)(it - tokens.begin()));
      }
      if (matched.size() != terms.size()) continue;
      for (size_t index : matched) spans.push_back(tokens[index].span);
    }
  }

  // A window of about fragment_size bytes around spans[anchor], grown to keep
  // every match whole and trimmed to whitespace and UTF-8 boundaries.
  Span windowAt(std::string_view value, size_t anchor) const {
    size_t size = (size_t)plan.fragmentSize;
    const Span& match = spans[anchor];
    size_t start = match.start - std::min(match.start, size / 4);
    size_t end = std::max(match.end, std::min(value.size(), start + size));
    start = std::min(start, end > size ? end - size : 0);
    // Grow over matches that straddle either edge.
    size_t first = anchor;
    while (first > 0 && spans[first - 1].end > start) start = std::min(start, spans[--first].start);
    while (first > 0 && spans[first - 1].start >= start) first--;
    size_t last = anchor;
    while (last + 1 < spans.size() && spans[last + 1].start < end) end = std::max(end, spans[++last].end);
    if (start > 0) {
      size_t limit = spans[first].start;
      size_t space = start;
      while (space < limit && !isSpace(value[space])) space++;
      if (space < limit) {
        start = space;
        while (start < limit && isSpace(value[start])) start++;
      } else {
        while (start < limit && isContinuation(value[start])) start++;
      }
    }
    if (end < value.size()) {
      size_t limit = spans[last].end;
      size_t space = end;
      while (space > limit && !isSpace(value[space - 1])) space--;
      if (space > limit) {
        end = space;
        while (end > limit && isSpace(value[end - 1])) end--;
      } else {
        while (end > limit && isContinuation(value[end])) end--;
      }
    }
    return {start, end};
  }

  void append(std::string& out, std::string_view text) const {
    if (!plan.html) {
      out += text;
      return;
    }
    for (char c : text) {
      switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#x27;"; break;
        default: out += c;
      }
    }
  }

  std::string render(std::string_view value, size_t start, size_t end) const {
    std::string out;
    size_t cursor = start;
    auto it = std::ranges::lower_bound(spans, start, {}, &Span::start);
    for (; it != spans.end() && it->end <= end; ++it) {
      append(out, value.substr(cursor, it->start - cursor));
      out += plan.preTag;
      append(out, value.substr(it->start, it->end - it->start));
      out += plan.postTag;
      cursor = it->end;
    }
    append(out, value.substr(cursor, end - cursor));
    return out;
  }
};

int32_t option(std::optional<int32_t> value, int32_t fallback, int32_t min, std::string_view name) {
  if (!value) return fallback;
  if (*value < min) {
    throw RequestError("highlight." + std::string(name) + " must be >= " + std::to_string(min),
                       "invalid_highlight");
  }
  return *value;
}

}  // namespace

bool HighlightTarget::matchesTerm(std::string_view token, std::string_view fitted) const {
  if (terms.contains(fitted)) return true;
  for (const Query* query : multiTerms) {
    switch (query->getKind()) {
      case QueryKind::PREFIX:
        if (token.starts_with(static_cast<const PrefixQuery*>(query)->getPrefix())) return true;
        break;
      case QueryKind::AUTOMATON:
        if (static_cast<const AutomatonQuery*>(query)->accepts(token)) return true;
        break;
      case QueryKind::FUZZY: {
        auto* fuzzy = static_cast<const FuzzyQuery*>(query);
        std::string_view term = fuzzy->getTerm();
        size_t prefix = (size_t)fuzzy->getPrefixLength();
        if (token.substr(0, prefix) == term.substr(0, prefix)
            && boundedEditDistance(token, term, fuzzy->getMaxEdits()) <= fuzzy->getMaxEdits()) {
          return true;
        }
        break;
      }
      default:
        break;
    }
  }
  return false;
}

HighlightPlan* buildHighlightPlan(SearchRequest& req, const api::Highlight& spec,
                                  std::span<Query* const> queries) {
  auto* plan = luxir::arenaCreate<HighlightPlan>(req.arena);
  plan->preTag = spec.pre_tag.value_or("<em>");
  plan->postTag = spec.post_tag.value_or("</em>");
  plan->fragmentSize = option(spec.fragment_size, 100, 1, "fragment_size");
  plan->numberOfFragments = option(spec.number_of_fragments, 5, 0, "number_of_fragments");
  plan->maxAnalyzedOffset = option(spec.max_analyzed_offset, 1000000, 1, "max_analyzed_offset");
  plan->html = spec.encoder == api::Highlight::Encoder::HTML;

  Targets leaves;
  for (const Query* query : queries) collectLeaves(query, leaves);
  ArenaResource mr(&req.arena);
  boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual> taken;
  // A stored root and the query leaves on any of its representations.
  auto add = [&](std::string_view outputKey, std::string_view name, bool named) {
    auto source = req.schema->resolveFor(name, OpClass::RETRIEVE);
    FieldType* stored = source.owner ? source.owner->primary.get() : source.fieldType;
    auto type = stored->type();
    bool text = type == FieldType::TEXT || type == FieldType::STRING || type == FieldType::ID;
    if (!text || !stored->isStored()) {
      if (!named) return;
      throw RequestError("Field '" + std::string(outputKey)
          + "' has no stored text to highlight", "invalid_field");
    }
    HighlightField field;
    field.outputKey = api::build::arenaStr(mr, outputKey);
    field.storedName = api::build::arenaStr(mr, source.logicalName);
    field.stored = stored;
    if (!taken.insert(field.storedName).second) return;
    for (const auto& [physical, leaf] : leaves) {
      if (rootOf(physical) != source.logicalName) continue;
      FieldType* type = req.schema->getFieldTypePtr(physical);
      if (type == nullptr) continue;
      field.targets.push_back(leaf);
      field.targets.back().fieldType = type;
    }
    if (!field.targets.empty()) plan->fields.push_back(std::move(field));
  };
  std::vector<std::string_view> patterns;
  for (std::string_view name : spec.fields) {
    if (name.find('*') == std::string_view::npos) add(name, name, true);
    else patterns.push_back(name);
  }
  if (spec.fields.empty()) patterns.push_back("*");
  for (std::string_view pattern : patterns) {
    for (const auto& [physical, leaf] : leaves) {
      std::string_view root = rootOf(physical);
      if (!globMatch(pattern, root)) continue;
      try {
        add(root, root, false);
      } catch (const RequestError&) {
        // Not a schema field (an engine field): nothing to highlight.
      }
    }
  }
  if (plan->fields.empty()) return nullptr;
  for (auto& field : plan->fields) field.storedInfos = req.reader->field(field.stored->storedResource_);
  return plan;
}

void highlightRows(SearchRequest& req, const HighlightPlan& plan,
                   std::span<const segdoc> segDocs, std::span<const uint32_t> rows,
                   api::Map* out, std::pmr::memory_resource& mr) {
  int32_t segNum = segDocs[rows[0]].segment();
  auto& postings = req.reader->segments()[(size_t)segNum].postingsReader();
  for (const auto& field : plan.fields) {
    auto* info = field.storedInfos.at((size_t)segNum);
    if (info == nullptr) continue;
    StoredFieldsReader sfr(postings, *info);
    int32_t fid = sfr.fieldId(field.storedName);
    if (fid < 0) continue;
    FieldHighlighter highlighter(plan, field);
    for (uint32_t row : rows) {
      sfr.readFieldById(segDocs[row].docId(), fid, [&](std::span<const std::string_view> values) {
        for (std::string_view value : values) highlighter.addValue(value);
      });
      auto fragments = highlighter.finish(mr);
      if (fragments.empty()) continue;
      auto* slot = api::build::mapSlot<api::Val>(out[row].fields, plan.fields.size(), field.outputKey, mr);
      slot->kind = api::ArrStr{fragments};
    }
  }
}

} // namespace luxir
