// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/analysis/CharFilter.h"
#include "luxir/analysis/AnalysisRegistry.h"
#include "luxir/analysis/HtmlEntities.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/screaming.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <uni_algo/conv.h>
#include <uni_algo/ranges_conv.h>

namespace luxir {

void CharFilter::replace(size_t start, size_t end, std::string_view replacement) {
  auto original = input.substr(start, end - start);
  if (original == replacement) return;
  buffer.append(input.substr(copied, start - copied));
  size_t outStart = buffer.size();
  buffer.append(replacement);
  // Single-byte substitutions preserve byte boundaries and need no edit.
  if (replacement.empty() && !edits.empty() && edits.back().inEnd == start &&
      edits.back().outStart == outStart && edits.back().outEnd == outStart) {
    edits.back().inEnd = end;
  } else if (!(end - start == 1 && replacement.size() == 1)) {
    edits.push_back({outStart, buffer.size(), start, end});
  }
  copied = end;
}

std::span<char> CharFilter::bytewiseOutput() {
  buffer.resize(input.size());
  copied = input.size();
  return {buffer.data(), buffer.size()};
}

std::string_view CharFilter::apply(std::string_view value) {
  input = value;
  copied = 0;
  buffer.clear();
  edits.clear();
  cursors = {};
  filter();
  if (!copied) return input;
  buffer.append(input.substr(copied));
  if (buffer.size() > (size_t) std::numeric_limits<int>::max())
    throw std::length_error("Cleaned text exceeds byte-offset limit");
  return buffer;
}

int CharFilter::correctOffset(int offset, bool end) {
  if (edits.empty()) return offset;
  auto& cursor = cursors[end];
  if (offset < cursor.offset) cursor.index = 0;
  cursor.offset = offset;
  size_t pos = (size_t) offset;
  if (end) {
    cursor.index = (size_t) screaming::gallopLowerBound((int64_t) cursor.index, (int64_t) edits.size(), pos,
        [&](int64_t i) { return edits[(size_t) i].outEnd; });
    if (cursor.index < edits.size()) {
      const auto& edit = edits[cursor.index];
      if (pos > edit.outStart) return (int) edit.inEnd;
      return (int) (edit.inStart - (edit.outStart - pos));
    }
  } else {
    cursor.index = (size_t) screaming::gallopLowerBound((int64_t) cursor.index, (int64_t) edits.size(), pos + 1,
        [&](int64_t i) { return edits[(size_t) i].outStart; });
    if (!cursor.index) return offset;
    const auto& edit = edits[cursor.index - 1];
    if (pos < edit.outEnd) return (int) edit.inStart;
    return (int) (edit.inEnd + (pos - edit.outEnd));
  }
  const auto& last = edits.back();
  return (int) (last.inEnd + (pos - last.outEnd));
}

namespace {
[[noreturn]] void badMapping(std::string_view reason) {
  throw std::invalid_argument("char filter 'mapping': " + std::string(reason));
}

std::string decodeMapping(std::string_view text) {
  auto whitespace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
  while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
  std::string out;
  auto hexScalar = [&](size_t& pos) {
    char32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos == text.size()) badMapping("incomplete Unicode escape in mappings");
      unsigned char c = (unsigned char) text[pos++];
      unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
      if (digit == 16) badMapping("invalid Unicode escape in mappings");
      cp = cp * 16 + digit;
    }
    return cp;
  };
  for (size_t pos = 0; pos < text.size();) {
    char c = text[pos++];
    if (c != '\\') { out.push_back(c); continue; }
    if (pos == text.size()) badMapping("trailing escape in mappings");
    switch (text[pos++]) {
      case '\\': out.push_back('\\'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        char32_t cp = hexScalar(pos);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (text.substr(pos, 2) != "\\u") badMapping("unpaired Unicode surrogate in mappings");
          pos += 2;
          char32_t low = hexScalar(pos);
          if (low < 0xDC00 || low > 0xDFFF) badMapping("unpaired Unicode surrogate in mappings");
          cp = 0x10000 + (cp - 0xD800) * 0x400 + low - 0xDC00;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          badMapping("unpaired Unicode surrogate in mappings");
        }
        out += una::utf32to8(std::u32string_view(&cp, 1));
        break;
      }
      default: badMapping("unknown escape in mappings; use \\\\, \\n, \\r, \\t, \\b, \\f or \\uXXXX");
    }
  }
  if (!una::is_valid_utf8(out)) badMapping("mappings must contain valid UTF-8");
  return out;
}

struct MappingTrie {
  struct Node {
    std::vector<std::pair<unsigned char, size_t>> edges;
    size_t replacement = std::string_view::npos;
  };
  std::bitset<256> first;
  std::vector<Node> nodes{1};
  std::vector<std::string> replacements;

  void add(std::string_view rule) {
    size_t split = std::string_view::npos;
    for (size_t i = 0; i < rule.size(); ++i) {
      if (rule[i] == '\\') { ++i; continue; }
      if (rule.substr(i, 2) != "=>") continue;
      if (split != std::string_view::npos) badMapping("each mapping needs exactly one unescaped =>");
      split = i++;
    }
    if (split == std::string_view::npos) badMapping("each mapping needs exactly one unescaped =>");
    auto source = decodeMapping(rule.substr(0, split));
    auto replacement = decodeMapping(rule.substr(split + 2));
    if (source.empty()) badMapping("mapping sources must not be empty");
    first.set((unsigned char) source.front());
    size_t node = 0;
    for (unsigned char c : source) {
      auto& edges = nodes[node].edges;
      auto it = std::find_if(edges.begin(), edges.end(), [c](auto edge) { return edge.first == c; });
      if (it == edges.end()) {
        size_t next = nodes.size();
        edges.emplace_back(c, next);
        nodes.emplace_back();
        node = next;
      } else {
        node = it->second;
      }
    }
    if (nodes[node].replacement != std::string_view::npos) badMapping("duplicate decoded mapping source");
    nodes[node].replacement = replacements.size();
    replacements.push_back(std::move(replacement));
  }

  size_t next(size_t node, unsigned char c) const {
    const auto& edges = nodes[node].edges;
    auto it = std::lower_bound(edges.begin(), edges.end(), c,
        [](auto edge, unsigned char value) { return edge.first < value; });
    return it != edges.end() && it->first == c ? it->second : std::string_view::npos;
  }
};

class MappingFilter : public CharFilter {
  std::shared_ptr<const MappingTrie> trie;
public:
  explicit MappingFilter(std::shared_ptr<const MappingTrie> trie) : trie(std::move(trie)) {}
private:
  void filter() override {
    // Schema-authored keys are short. Forward leftmost-longest matching costs
    // O(input bytes * longest key bytes) in the worst case; replacements are not rescanned.
    for (size_t pos = 0; pos < input.size();) {
      if (!trie->first[(unsigned char) input[pos]]) { ++pos; continue; }
      size_t node = 0, end = pos, replacement = std::string_view::npos;
      for (size_t scan = pos; scan < input.size(); ++scan) {
        node = trie->next(node, (unsigned char) input[scan]);
        if (node == std::string_view::npos) break;
        if (trie->nodes[node].replacement != std::string_view::npos) {
          replacement = trie->nodes[node].replacement;
          end = scan + 1;
        }
      }
      if (replacement == std::string_view::npos) { ++pos; continue; }
      replace(pos, end, trie->replacements[replacement]);
      pos = end;
    }
  }
};

bool alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? (char) (c + ('a' - 'A')) : c; }
bool equalName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](char x, char y) { return lower(x) == y; });
}

// A bounded, forward-only text extractor, not a browser DOM or an HTML sanitizer.
// No recursion, regex backtracking, external resources, or entity expansion.
class HtmlStripFilter : public CharFilter {
  std::string entity;
  size_t noTagAfter = 0;
  std::array<size_t, 2> noQuoteAfter{};
  // Lucene's inline elements. Other names, including unknown elements, separate text.
  static constexpr std::string_view inlineElements[] = {
    "a", "abbr", "acronym", "b", "basefont", "bdo", "big", "cite", "code", "dfn", "em", "font",
    "i", "img", "input", "kbd", "label", "q", "s", "samp", "select", "small", "span", "strike",
    "strong", "sub", "sup", "textarea", "tt", "u", "var"};

  size_t quotedEnd(size_t pos, char quote) {
    auto& missing = noQuoteAfter[quote == '"'];
    if (pos >= missing) return std::string_view::npos;
    size_t end = input.find(quote, pos);
    if (end == std::string_view::npos) { missing = pos; return end; }
    return end + 1;
  }

  size_t tagEnd(size_t pos) {
    if (pos >= noTagAfter) return std::string_view::npos;
    enum State { ATTRIBUTES, BEFORE_VALUE, UNQUOTED } state = ATTRIBUTES;
    size_t tail = pos;
    while (pos < input.size()) {
      char c = input[pos++];
      if (c == '>') return pos;
      // A nested opener outside a quoted value rejects this candidate early.
      // Quote searches jump to the next matching quote; failed searches are
      // cached, so malformed candidates cannot repeatedly scan a suffix to EOF.
      if (c == '<') return std::string_view::npos;
      if (state == BEFORE_VALUE) {
        if (space(c)) continue;
        if (c == '\'' || c == '"') {
          pos = quotedEnd(pos, c);
          if (pos == std::string_view::npos) return pos;
          tail = pos;
          state = ATTRIBUTES;
        } else {
          state = UNQUOTED;
        }
      } else if (state == UNQUOTED) {
        if (space(c)) state = ATTRIBUTES;
      } else if (c == '=') {
        state = BEFORE_VALUE;
      }
    }
    // Since the last quoted value, there was no raw '<' or '>'.
    noTagAfter = std::min(noTagAfter, tail);
    return std::string_view::npos;
  }

  size_t declarationEnd(size_t pos) {
    while (pos < input.size()) {
      char c = input[pos++];
      if (c == '>') return pos;
      if (c == '\'' || c == '"') {
        pos = quotedEnd(pos, c);
        if (pos == std::string_view::npos) return input.size();
      }
    }
    return pos;
  }

  size_t rawEnd(size_t pos, std::string_view name) {
    while ((pos = input.find('<', pos)) != std::string_view::npos) {
      size_t end = pos + 2 + name.size();
      if (end < input.size() && input[pos + 1] == '/' &&
          equalName(input.substr(pos + 2, name.size()), name) &&
          (space(input[end]) || input[end] == '/' || input[end] == '>')) {
        end = tagEnd(end);
        if (end != std::string_view::npos) return end;
      }
      ++pos;
    }
    return input.size();
  }

  size_t reference(size_t pos) {
    size_t start = pos++;
    if (pos == input.size()) return start;
    if (input[pos] == '#') {
      ++pos;
      unsigned base = 10;
      if (pos < input.size() && (input[pos] == 'x' || input[pos] == 'X')) { base = 16; ++pos; }
      size_t firstDigit = pos;
      uint32_t cp = 0;
      for (; pos < input.size(); ++pos) {
        char c = lower(input[pos]);
        unsigned n = digit(c) ? (unsigned) (c - '0') : c >= 'a' && c <= 'f' ? (unsigned) (c - 'a' + 10) : 16;
        if (n >= base) break;
        // Saturate on overflow but keep consuming digits in linear time.
        cp = cp > 0x10FFFF / base ? 0x110000 : cp * base + n;
      }
      if (pos == firstDigit) return start;
      if (pos < input.size() && input[pos] == ';') ++pos;
      if (!cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
      // HTML's legacy numeric references in the C1 range use Windows-1252.
      static constexpr char32_t c1[] = {
        0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
        0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
      char32_t scalar = cp >= 0x80 && cp <= 0x9F ? c1[cp - 0x80] : (char32_t) cp;
      entity = una::utf32to8(std::u32string_view(&scalar, 1));
      return pos;
    }
    size_t first = pos;
    while (pos < input.size() && pos - first < html_entities::maxNameLength &&
           (alpha(input[pos]) || digit(input[pos]))) ++pos;
    if (pos < input.size() && input[pos] == ';') ++pos;
    // Longest match also handles the standard's legacy semicolonless names.
    for (; pos > first; --pos) {
      auto name = input.substr(first, pos - first);
      auto it = std::lower_bound(std::begin(html_entities::entries), std::end(html_entities::entries), name,
          [](const auto& entry, std::string_view key) { return entry.name < key; });
      if (it != std::end(html_entities::entries) && it->name == name) {
        entity.assign(it->value);
        return pos;
      }
    }
    return start;
  }

  void filter() override {
    noTagAfter = input.size();
    noQuoteAfter.fill(input.size());
    for (size_t pos = 0; pos < input.size();) {
      size_t start = pos;
      if (input[pos] == '&') {
        size_t end = reference(pos);
        if (end > pos) { replace(pos, end, entity); pos = end; continue; }
      } else if (input[pos] == '<') {
        if (input.substr(pos).starts_with("<![CDATA[")) {
          replace(pos, pos + 9, "");
          pos = input.find("]]>", pos + 9);
          if (pos == std::string_view::npos) pos = input.size();
          else { replace(pos, pos + 3, ""); pos += 3; }
          continue;
        }
        if (input.substr(pos).starts_with("<!--")) {
          pos += 4;
          while ((pos = input.find("--", pos)) != std::string_view::npos) {
            if (input.substr(pos).starts_with("-->")) { pos += 3; break; }
            if (input.substr(pos).starts_with("--!>")) { pos += 4; break; }
            ++pos;
          }
          if (pos == std::string_view::npos) pos = input.size();
          replace(start, pos, "");
          continue;
        }
        if (pos + 1 < input.size() && (input[pos + 1] == '!' || input[pos + 1] == '?')) {
          pos = declarationEnd(pos + 2);
          replace(start, pos, "");
          continue;
        }
        size_t nameStart = pos + 1;
        bool closing = nameStart < input.size() && input[nameStart] == '/';
        if (closing) ++nameStart;
        if (nameStart < input.size() && alpha(input[nameStart])) {
          size_t nameEnd = nameStart + 1;
          while (nameEnd < input.size() && (alpha(input[nameEnd]) || digit(input[nameEnd]) ||
                 input[nameEnd] == '-' || input[nameEnd] == '_' || input[nameEnd] == ':')) ++nameEnd;
          auto name = input.substr(nameStart, nameEnd - nameStart);
          size_t end = tagEnd(nameEnd);
          if (end != std::string_view::npos) {
            if (!closing && (equalName(name, "script") || equalName(name, "style")))
              end = rawEnd(end, equalName(name, "script") ? "script" : "style");
            bool inlineElement = std::any_of(std::begin(inlineElements), std::end(inlineElements),
                [&](std::string_view element) { return equalName(name, element); });
            replace(start, end, inlineElement ? "" : "\n");
            pos = end;
            continue;
          }
        }
      }
      ++pos;
    }
  }
};
} // namespace

namespace {
std::unique_ptr<const CharFilterFactory> mappingCharFilter(const api::AnalyzerComponent& def) {
  struct Factory : CharFilterFactory {
    std::shared_ptr<const MappingTrie> trie;
    std::unique_ptr<CharFilter> create() const override { return std::make_unique<MappingFilter>(trie); }
  };
  auto trie = std::make_shared<MappingTrie>();
  bool found = false;
  for (const auto& [key, value] : def.params) {
    if (key != "mappings") badMapping("unknown parameter '" + std::string(key) + "'; valid parameters: mappings");
    found = true;
    if (auto* list = std::get_if<api::ArrStr>(&value->kind)) {
      for (auto rule : list->v) trie->add(rule);
    } else if (auto* list = std::get_if<api::ArrVal>(&value->kind)) {
      for (const auto& item : list->v) {
        auto* rule = std::get_if<std::string_view>(&item.kind);
        if (!rule) badMapping("mappings must be a list of strings");
        trie->add(*rule);
      }
    } else {
      badMapping("mappings must be a list of strings");
    }
  }
  if (!found) badMapping("missing required parameter 'mappings'");
  for (auto& node : trie->nodes) std::sort(node.edges.begin(), node.edges.end());
  auto factory = std::make_unique<Factory>();
  factory->name = def.name;
  factory->normalizing = true;
  factory->trie = std::move(trie);
  return factory;
}

template <std::unique_ptr<CharFilter> (*Make)(), bool Normalizing>
std::unique_ptr<const CharFilterFactory> plainCharFilter(const api::AnalyzerComponent& def) {
  if (!def.params.empty()) throw std::invalid_argument("char filter '" + std::string(def.name) + "' takes no parameters");
  struct Factory : CharFilterFactory {
    std::unique_ptr<CharFilter> create() const override { return Make(); }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = def.name;
  factory->normalizing = Normalizing;
  return factory;
}

std::unique_ptr<CharFilter> makeHtmlStripFilter() { return std::make_unique<HtmlStripFilter>(); }
} // namespace

void AnalysisRegistry::registerBuiltinCharFilters() {
  addCharFilter("nfkc_cf", plainCharFilter<makeNfkcCfCharFilter, true>);
  addCharFilter("mapping", mappingCharFilter);
  addCharFilter("html_strip", plainCharFilter<makeHtmlStripFilter, false>);
}
} // namespace luxir
