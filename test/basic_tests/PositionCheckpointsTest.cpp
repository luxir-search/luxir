// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <deque>
#include <random>

#include "luxir/analysis/AnalysisRegistry.h"
#include "luxir/analysis/Analyzer.h"
#include "luxir/analysis/PositionCheckpoints.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/schema/Schema.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// A two-word synonym rule with Lucene-style lookahead: "new york" also emits
// "ny" stacked on "new". Buffers tokens, so it must report resumable() itself:
// only a token that starts a fresh match attempt, with nothing pending, is.
class NewYorkFilter : public TokenFilter {
  struct Buffered { std::string text; int increment, start, end; bool fresh; };
  std::deque<Buffered> out;
  std::optional<Buffered> ahead;
  std::string text;
  bool fresh = false;

  bool pull(Buffered& b) {
    if (ahead) { b = std::move(*ahead); ahead.reset(); return true; }
    if (!source().incrementToken()) return false;
    b = {std::string(token.text), token.positionIncrement, token.startOffset, token.endOffset, source().resumable()};
    return true;
  }
public:
  using TokenFilter::TokenFilter;
  void reset() override { TokenFilter::reset(); out.clear(); ahead.reset(); }
  bool resumable() override { return fresh; }
  bool incrementToken() override {
    if (out.empty()) {
      Buffered first, next;
      if (!pull(first)) return false;
      bool match = first.text == "new" && pull(next);
      if (match && next.text != "york") { ahead = std::move(next); match = false; }
      out.push_back(first);
      if (match) {
        out.push_back({"ny", 0, first.start, next.end, false});
        next.fresh = false;
        out.push_back(std::move(next));
      }
    }
    auto b = std::move(out.front());
    out.pop_front();
    text = std::move(b.text);
    token.text = text;
    token.positionIncrement = b.increment;
    token.setOffset(b.start, b.end);
    fresh = b.fresh;
    return true;
  }
};

template <bool Resumable>
std::unique_ptr<TokenFilterFactory> newYork(const api::AnalyzerComponent& def) {
  struct Factory : TokenFilterFactory {
    std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
      return std::make_unique<NewYorkFilter>(std::move(source));
    }
  };
  auto factory = std::make_unique<Factory>();
  factory->name = def.name;
  factory->stateful = true;
  factory->resumable = Resumable;
  return factory;
}

const AnalysisRegistry& testRegistry() {
  static AnalysisRegistry* registry = [] {
    auto* r = new AnalysisRegistry();
    r->addFilter("new_york", newYork<true>);
    r->addFilter("new_york_unaware", newYork<false>);
    return r;
  }();
  return *registry;
}

std::shared_ptr<const Analyzer> compile(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  std::string error;
  if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
  return Analyzer::compile(def, testRegistry());
}

// Text that exercises the chains: stop words, the synonym, punctuation that
// joins or splits words, markup and entities, mapped sequences, accents,
// combining marks, CJK and varied whitespace.
std::string text(uint32_t seed, size_t words) {
  static const char* pieces[] = {
    "the", "a", "of", "and", "to", "new", "york", "New York", "Phone", "graph", "ab", "bc", "abc",
    "can't", "U.S.A.", "3.14", "e-mail", "foo_bar", "<b>bold</b>", "<i>it</i>alic", "x<br>y", "&amp;",
    "&lt;tag&gt;", "<!-- note -->", "<![CDATA[ a <b> c ]]>", "<![CDATA[x <b>y</b> &amp;]]>", "<a href='q > r'>link</a>", "caf\xc3\xa9",
    "Cafe\xcc\x81", "\xef\xac\x81" "ne", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "STRASSE", "Stra\xc3\x9f" "e",
    "\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0", "--", "...", "&&", "1,000", "\xcc\x81", "running"};
  static const char* spaces[] = {" ", " ", " ", "  ", "\n", "\t", "\r\n", ", ", ". ", ""};
  std::mt19937 rng(seed);
  std::string out;
  for (size_t i = 0; i < words; ++i) {
    out += pieces[rng() % std::size(pieces)];
    out += spaces[rng() % std::size(spaces)];
  }
  return out;
}

struct Tok {
  uint32_t value;
  int32_t position;
  int start, end;
  std::string text;
  bool operator==(const Tok&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Tok& t) {
  return os << "{v" << t.value << " p" << t.position << " [" << t.start << "," << t.end << ") '" << t.text << "'}";
}

std::vector<Tok> drain(FieldTokenStream& stream, size_t limit = SIZE_MAX) {
  std::vector<Tok> out;
  while (out.size() < limit && stream.next())
    out.push_back({stream.value, stream.position, stream.startOffset, stream.endOffset, std::string(stream.token().text)});
  return out;
}

// Every point a chain reports resumable while analyzing `values` the way the
// indexer does, plus each value's first token, with the full token index.
struct Point { PositionCheckpoint checkpoint; size_t token; };
std::vector<Point> resumePoints(TokenChain& chain, std::span<const std::string_view> values) {
  std::vector<Point> points;
  int position = -1;
  size_t tokens = 0;
  for (uint32_t v = 0; v < values.size(); ++v) {
    if (v) position += FieldTokenStream::POSITION_INCREMENT_GAP;
    chain.setValue(values[v]);
    chain.reset();
    bool first = true;
    while (chain.tail->incrementToken()) {
      const auto& token = chain.getToken();
      position += token.positionIncrement;
      int offset = first ? 0 : token.positionIncrement > 0 ? chain.resumeOffset() : -1;
      if (offset >= 0) points.push_back({{v, (uint32_t) offset, position}, tokens});
      first = false;
      ++tokens;
    }
    position += chain.end();
  }
  return points;
}

// Resuming at any point the chain reports yields the full analysis's tokens
// from there, and stopping at a later point yields exactly the span between.
// Returns the number of mid-value points checked.
size_t expectEquivalent(const Analyzer& analyzer, std::span<const std::string_view> values) {
  auto chain = analyzer.createChain();
  FieldTokenStream stream(*chain, values);
  stream.start();
  auto full = drain(stream);
  auto points = resumePoints(*chain, values);
  size_t inside = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& [from, index] = points[i];
    SCOPED_TRACE(::testing::Message() << "resume at value " << from.value << " offset " << from.offset);
    if (from.offset) {
      ++inside;
      EXPECT_EQ(from.offset, (uint32_t) full[index].start);
    }
    EXPECT_EQ(from.position, full[index].position);
    // A bounded check from every point, the whole tail from some.
    stream.start(from);
    size_t limit = i % 16 == 0 ? SIZE_MAX : 24;
    auto resumed = drain(stream, limit);
    auto expected = std::vector<Tok>(full.begin() + (ptrdiff_t) index,
        full.begin() + (ptrdiff_t) (index + std::min(full.size() - index, limit)));
    EXPECT_EQ(expected, resumed);
    if (i + 3 < points.size()) {
      const auto& until = points[i + 3];
      stream.start(from);
      stream.stopAt(until.checkpoint);
      EXPECT_EQ(std::vector<Tok>(full.begin() + (ptrdiff_t) index, full.begin() + (ptrdiff_t) until.token),
                drain(stream));
    }
  }
  return inside;
}

constexpr std::string_view STOP = R"({"name":"stop","params":{"words":["the","a","of","and","to"]}})";

std::string chainJson(std::string_view head, std::string_view filters, std::string_view charFilters = "") {
  std::string json = "{\"tokenizer\":\"" + std::string(head) + "\",\"filters\":[" + std::string(filters) + "]";
  if (!charFilters.empty()) json += ",\"char_filters\":[" + std::string(charFilters) + "]";
  return json + "}";
}

}  // namespace

class PositionCheckpointsTest : public LuxirTest {};

TEST_F(PositionCheckpointsTest, encodingRoundTripAndLookup) {
  std::vector<PositionCheckpoint> list{{0, 0, 130}, {0, 700, 260}, {2, 0, 500}, {2, 9, 640}, {3, 5, 70000}};
  std::string bytes;
  PositionCheckpoints::encode(list, bytes);
  std::vector<PositionCheckpoint> decoded;
  PositionCheckpoints::decode(bytes, decoded);
  EXPECT_EQ(list, decoded);
  EXPECT_EQ(nullptr, PositionCheckpoints::atOrBefore(list, 129));
  EXPECT_EQ(&list[0], PositionCheckpoints::atOrBefore(list, 130));
  EXPECT_EQ(&list[1], PositionCheckpoints::atOrBefore(list, 499));
  EXPECT_EQ(&list[4], PositionCheckpoints::atOrBefore(list, INT32_MAX));
  EXPECT_EQ(&list[2], PositionCheckpoints::after(list, 260));
  EXPECT_EQ(nullptr, PositionCheckpoints::after(list, 70000));
  bytes.pop_back();
  EXPECT_THROW(PositionCheckpoints::decode(bytes, decoded), std::runtime_error);
  EXPECT_THROW(PositionCheckpoints::decode("\x80\x80\x80\x80\x80\x80", decoded), std::runtime_error);
  EXPECT_THROW(PositionCheckpoints::decode(std::string_view("\x00\x00\x80\x80\x80\x80\x10", 7), decoded), std::runtime_error);
  // Positions or offsets that go backwards are corrupt.
  EXPECT_THROW(PositionCheckpoints::decode(std::string("\x00\x05\x00\x00\xff\xff\xff\xff\x0f\x00", 10), decoded),
               std::runtime_error);

  std::string value = "one two three";
  std::vector<std::string_view> values{value};
  auto chain = compile(chainJson("whitespace", ""))->createChain();
  FieldTokenStream stream(*chain, values);
  EXPECT_THROW(stream.start(PositionCheckpoint{1, 0, 5}), std::runtime_error);
  EXPECT_THROW(stream.start(PositionCheckpoint{0, 14, 5}), std::runtime_error);
  stream.start(PositionCheckpoint{0, 4, 1});
  ASSERT_TRUE(stream.next());
  EXPECT_EQ("two", stream.token().text);
  EXPECT_EQ(4, stream.startOffset);
  EXPECT_THROW(stream.stopAt(PositionCheckpoint{0, 8, 2}), std::logic_error);
}

TEST_F(PositionCheckpointsTest, resumedAnalysisMatchesFullAnalysis) {
  std::string body = text(1, 1500), second = text(2, 300), third = text(3, 40);
  std::vector<std::string_view> values{body, second, "", third};
  for (const auto& json : {
      chainJson("unicode_word", "\"nfkc_cf\",\"fold\",\"kstem\"," + std::string(STOP)),
      chainJson("unicode_word", "\"lowercase\"," + std::string(STOP)),
      chainJson("unicode_word", "\"nfkc_cf\",\"new_york\"," + std::string(STOP), "\"html_strip\""),
      chainJson("whitespace", "\"lowercase\",\"new_york\"," + std::string(STOP),
          R"("html_strip",{"name":"mapping","params":{"mappings":["ph => f","&& => and","ab => ab","bc => X"]}},"nfkc_cf")"),
      chainJson("whitespace", "\"nfkc_cf\""),
      chainJson("keyword", "")}) {
    SCOPED_TRACE(json);
    auto analyzer = compile(json);
    EXPECT_TRUE(analyzer->resumable);
    size_t inside = expectEquivalent(*analyzer, values);
    if (analyzer->tokenizer->name != "keyword") {
      EXPECT_GT(inside, 300u);
    }
  }
}

TEST_F(PositionCheckpointsTest, chainsThatCannotResumeSaySo) {
  auto analyzer = compile(chainJson("whitespace", "\"new_york_unaware\""));
  EXPECT_FALSE(analyzer->resumable);
  std::string body = text(4, 400);
  std::vector<std::string_view> values{body, body};
  auto chain = analyzer->createChain();
  for (const auto& point : resumePoints(*chain, values)) EXPECT_EQ(0u, point.checkpoint.offset);
  expectEquivalent(*analyzer, values);
  // Resuming inside CDATA content or inside a replacement would change the
  // text: refused.
  analyzer = compile(chainJson("whitespace", "",
      R"("html_strip",{"name":"mapping","params":{"mappings":["ab => a b"]}})"));
  chain = analyzer->createChain();
  chain->setValue("<![CDATA[one two]]> three x ab y");
  chain->reset();
  std::vector<std::pair<std::string, int>> seen;
  while (chain->tail->incrementToken()) seen.emplace_back(chain->getToken().text, chain->resumeOffset());
  EXPECT_EQ((std::vector<std::pair<std::string, int>>{
      {"one", -1}, {"two", -1}, {"three", 20}, {"x", 26}, {"a", 28}, {"b", -1}, {"y", 31}}), seen);
}

TEST_F(PositionCheckpointsTest, indexedCheckpointsResumeStoredValues) {
  CollectionHelper helper;
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  std::string json = R"({"fields":{"id":"id",
    "body":{"type":"text","stored":true,"analyzer":)"
      + chainJson("unicode_word", "\"nfkc_cf\",\"fold\",\"kstem\"," + std::string(STOP)) + R"(,
      "variants":{"plain":{"type":"text","analyzer":{"tokenizer":"whitespace","char_filters":["html_strip"]}}}},
    "tags":{"type":"text","multi":true,"stored":true},
    "unstored":{"type":"text","stored":false}}})";
  ASSERT_TRUE(api::read_json(def, json, arena));
  helper.collection().setSchema(Schema::fromProto(def));

  std::string longBody = text(5, 3000), shortBody = text(6, 50);
  std::vector<std::string> tags;
  for (uint32_t i = 0; i < 12; ++i) tags.push_back(text(10 + i, 60));
  ASSERT_TRUE(helper.index(flatdoc("id", "long", "body", longBody, "tags", tags, "unstored", longBody)).success);
  // Many short values: gaps put them far apart in positions, but they hold
  // too few tokens for a checkpoint.
  std::vector<std::string> shortTags(60, "red blue");
  ASSERT_TRUE(helper.index(flatdoc("id", "short", "body", shortBody, "tags", shortTags), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "later", "body", text(7, 2000)), UpdateMessage::COMMIT).success);

  auto check = [&](size_t expectedSegments) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(expectedSegments, reader->segments().size());
    auto& postings = reader->segments()[0].postingsReader();
    auto* info = postings.fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
    ASSERT_NE(nullptr, info);
    StoredFieldsReader stored(postings, *info);
    auto schema = helper.collection().getSchema();
    EXPECT_GT(0, stored.fieldId(PositionCheckpoints::storedName("unstored")));
    std::vector<PositionCheckpoint> list;
    for (auto field : {"body", "body__plain", "tags"}) {
      SCOPED_TRACE(field);
      const auto& analyzer = *static_cast<TextFieldType&>(*schema->getFieldTypeEx(field)).analyzer_;
      int32_t id = stored.fieldId(PositionCheckpoints::storedName(field));
      int32_t valuesId = stored.fieldId(std::string_view(field).starts_with("body") ? "body" : "tags");
      ASSERT_LE(0, id);
      // Doc 1 is short: nothing recorded.
      EXPECT_FALSE(PositionCheckpoints::read(stored, 1, id, list));
      ASSERT_TRUE(PositionCheckpoints::read(stored, 0, id, list));
      std::vector<std::string> copies;
      stored.readFieldById(0, valuesId, [&](std::span<const std::string_view> values) {
        copies.assign(values.begin(), values.end());
      });
      std::vector<std::string_view> values(copies.begin(), copies.end());
      size_t bytes = 0;
      for (auto v : values) bytes += v.size();
      std::string encoded;
      PositionCheckpoints::encode(list, encoded);
      EXPECT_LT(encoded.size() * 100, bytes * 2) << "checkpoints cost " << encoded.size() << " of " << bytes;

      auto chain = analyzer.createChain();
      FieldTokenStream stream(*chain, values);
      stream.start();
      auto full = drain(stream);
      EXPECT_GT(list.size(), full.size() / (2 * PositionCheckpoints::INTERVAL));
      for (size_t i = 0; i < list.size(); ++i) {
        const auto& c = list[i];
        if (i) {
          EXPECT_GE(c.position - list[i - 1].position, PositionCheckpoints::INTERVAL);
        }
        auto at = std::find_if(full.begin(), full.end(), [&](const Tok& t) { return t.position == c.position; });
        ASSERT_NE(full.end(), at);
        EXPECT_EQ(c.value, at->value);
        if (c.offset) {
          EXPECT_EQ(c.offset, (uint32_t) at->start);
        }
        // The lookup a reader does for a position just after the checkpoint.
        EXPECT_EQ(&c, PositionCheckpoints::atOrBefore(list, c.position + 5));
        stream.start(c);
        if (i + 1 < list.size()) stream.stopAt(list[i + 1]);
        auto end = i + 1 < list.size()
            ? std::find_if(full.begin(), full.end(), [&](const Tok& t) { return t.position == list[i + 1].position; })
            : full.end();
        EXPECT_EQ(std::vector<Tok>(at, end), drain(stream));
      }
    }
  };
  check(2);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  check(1);
}
