// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/analysis/CharFilter.h"
#include "luxir/analysis/HtmlEntities.h"
#include "luxir/api/luxir_types.hpp"

#include <algorithm>
#include <array>
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
  // ASCII case changes preserve byte boundaries exactly and need no edit.
  if (replacement.empty() && !edits.empty() && edits.back().inEnd == start &&
      edits.back().outStart == outStart && edits.back().outEnd == outStart) {
    edits.back().inEnd = end;
  } else if (!(end - start == 1 && replacement.size() == 1)) {
    edits.push_back({outStart, buffer.size(), start, end});
  }
  copied = end;
}

std::string_view CharFilter::apply(std::string_view value) {
  input = value;
  copied = 0;
  buffer.clear();
  edits.clear();
  filter();
  if (!copied) return input;
  buffer.append(input.substr(copied));
  if (buffer.size() > (size_t) std::numeric_limits<int>::max())
    throw std::length_error("Cleaned text exceeds byte-offset limit");
  return buffer;
}

int CharFilter::correctOffset(int offset, bool end) const {
  size_t pos = (size_t) offset;
  if (end) {
    auto it = std::lower_bound(edits.begin(), edits.end(), pos,
        [](const Edit& edit, size_t p) { return edit.outEnd < p; });
    if (it != edits.end()) {
      if (pos > it->outStart) return (int) it->inEnd;
      return (int) (it->inStart - (it->outStart - pos));
    }
  } else {
    auto it = std::upper_bound(edits.begin(), edits.end(), pos,
        [](size_t p, const Edit& edit) { return p < edit.outStart; });
    if (it == edits.begin()) return offset;
    --it;
    if (pos < it->outEnd) return (int) it->inStart;
    return (int) (it->inEnd + (pos - it->outEnd));
  }
  if (edits.empty()) return offset;
  const auto& last = edits.back();
  return (int) (last.inEnd + (pos - last.outEnd));
}

namespace {
class RemoveCharsFilter : public CharFilter {
  std::shared_ptr<const std::vector<char32_t>> chars;
  std::array<bool, 128> ascii{};
public:
  explicit RemoveCharsFilter(std::shared_ptr<const std::vector<char32_t>> chars) : chars(std::move(chars)) {
    for (char32_t cp : *this->chars) if (cp < ascii.size()) ascii[cp] = true;
  }
private:
  void filter() override {
    auto decoded = una::views::utf8(input);
    for (auto it = decoded.begin(); it != decoded.end(); ++it) {
      char32_t cp = *it;
      if (cp < ascii.size() ? ascii[cp] : std::binary_search(chars->begin(), chars->end(), cp)) {
        replace((size_t) (it.begin() - input.begin()), (size_t) (it.end() - input.begin()), "");
      }
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

  size_t tagEnd(size_t pos) const {
    char quote = 0;
    for (; pos < input.size(); ++pos) {
      char c = input[pos];
      if (quote) { if (c == quote) quote = 0; }
      else if (c == '\'' || c == '"') quote = c;
      else if (c == '>') return pos + 1;
    }
    return input.size(); // an unterminated tag consumes the remaining markup
  }

  size_t rawEnd(size_t pos, std::string_view name) const {
    while ((pos = input.find('<', pos)) != std::string_view::npos) {
      size_t end = pos + 2 + name.size();
      if (end < input.size() && input[pos + 1] == '/' &&
          equalName(input.substr(pos + 2, name.size()), name) &&
          (space(input[end]) || input[end] == '/' || input[end] == '>')) return tagEnd(end);
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
    for (size_t pos = 0; pos < input.size();) {
      size_t start = pos;
      if (input[pos] == '&') {
        size_t end = reference(pos);
        if (end > pos) { replace(pos, end, entity); pos = end; continue; }
      } else if (input[pos] == '<') {
        if (input.substr(pos).starts_with("<!--")) {
          pos += 4;
          while ((pos = input.find("--", pos)) != std::string_view::npos) {
            if (input.substr(pos).starts_with("-->")) { pos += 3; break; }
            if (input.substr(pos).starts_with("--!>")) { pos += 4; break; }
            ++pos;
          }
          if (pos == std::string_view::npos) pos = input.size();
        } else if (pos + 1 < input.size() && (input[pos + 1] == '!' || input[pos + 1] == '?')) {
          pos = tagEnd(pos + 2);
        } else {
          size_t nameStart = pos + 1;
          bool closing = nameStart < input.size() && input[nameStart] == '/';
          if (closing) ++nameStart;
          if (nameStart < input.size() && alpha(input[nameStart])) {
            size_t nameEnd = nameStart;
            while (nameEnd < input.size() && !space(input[nameEnd]) &&
                   input[nameEnd] != '/' && input[nameEnd] != '>') ++nameEnd;
            auto name = input.substr(nameStart, nameEnd - nameStart);
            pos = tagEnd(nameEnd);
            if (!closing && (equalName(name, "script") || equalName(name, "style")))
              pos = rawEnd(pos, equalName(name, "script") ? "script" : "style");
          }
        }
        if (pos > start) {
          // A separator for every tag prevents adjacent elements merging words.
          replace(start, pos, " ");
          continue;
        }
      }
      ++pos;
    }
  }
};
} // namespace

std::unique_ptr<const CharFilterFactory> makeCharFilterFactory(const api::AnalyzerComponent& def) {
  constexpr std::string_view valid = "lowercase, nfkc_cf, fold, remove_chars, html_strip";
  if (def.name == "remove_chars") {
    struct Factory : CharFilterFactory {
      std::shared_ptr<const std::vector<char32_t>> chars;
      std::unique_ptr<CharFilter> create() const override { return std::make_unique<RemoveCharsFilter>(chars); }
    };
    auto chars = std::make_shared<std::vector<char32_t>>();
    bool found = false;
    for (const auto& [key, value] : def.params) {
      if (key != "chars") throw std::invalid_argument("char filter 'remove_chars': unknown parameter '" +
          std::string(key) + "'; valid parameters: chars");
      found = true;
      auto* list = std::get_if<api::ArrStr>(&value->kind);
      if (auto* empty = std::get_if<api::ArrVal>(&value->kind); empty && empty->v.empty()) continue;
      if (!list) throw std::invalid_argument("char filter 'remove_chars': chars must be a list of single Unicode characters");
      for (auto item : list->v) {
        auto scalars = una::utf8to32(item);
        if (scalars.size() != 1 || una::utf32to8(scalars) != item)
          throw std::invalid_argument("char filter 'remove_chars': each chars entry must be one valid Unicode scalar");
        chars->push_back(scalars[0]);
      }
    }
    if (!found) throw std::invalid_argument("char filter 'remove_chars': missing required parameter 'chars'");
    std::sort(chars->begin(), chars->end());
    chars->erase(std::unique(chars->begin(), chars->end()), chars->end());
    auto factory = std::make_unique<Factory>();
    factory->name = def.name;
    factory->chars = std::move(chars);
    return factory;
  }
  if (def.name == "html_strip" || def.name == "lowercase" || def.name == "nfkc_cf" || def.name == "fold") {
    if (!def.params.empty()) throw std::invalid_argument("char filter '" + std::string(def.name) + "' takes no parameters");
    struct Factory : CharFilterFactory {
      std::unique_ptr<CharFilter> create() const override {
        if (name == "html_strip") return std::make_unique<HtmlStripFilter>();
        return makeUnicodeCharFilter(name);
      }
    };
    auto factory = std::make_unique<Factory>();
    factory->name = def.name;
    return factory;
  }
  throw std::invalid_argument("unknown char filter '" + std::string(def.name) + "'; valid char filters: " + std::string(valid));
}
} // namespace luxir
