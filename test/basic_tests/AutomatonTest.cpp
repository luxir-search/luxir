#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "solux/util/automaton/ByteDfa.h"
#include "solux/util/automaton/Utf32ToUtf8.h"
#include "solux/util/automaton/WildcardCompiler.h"
#include "solux/util/automaton/RegExpParser.h"

using namespace solux::automaton;

static bool accepts(const Automaton& automaton, std::string_view bytes) {
  std::vector<int32_t> current{0};
  for (unsigned char byte : bytes) {
    std::vector<int32_t> next;
    for (int32_t state : current) {
      for (Transition transition : automaton.transitionsFrom(state)) {
        if (transition.min <= byte && byte <= transition.max) next.push_back(transition.dest);
      }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    current.swap(next);
  }
  for (int32_t state : current) if (automaton.accept(state)) return true;
  return false;
}

static std::string utf8(int32_t codepoint) {
  std::string bytes;
  if (codepoint < 0x80) bytes.push_back((char)codepoint);
  else if (codepoint < 0x800) {
    bytes.push_back((char)(0xc0 | (codepoint >> 6)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  } else if (codepoint < 0x10000) {
    bytes.push_back((char)(0xe0 | (codepoint >> 12)));
    bytes.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  } else {
    bytes.push_back((char)(0xf0 | (codepoint >> 18)));
    bytes.push_back((char)(0x80 | ((codepoint >> 12) & 0x3f)));
    bytes.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
    bytes.push_back((char)(0x80 | (codepoint & 0x3f)));
  }
  return bytes;
}

static void strings(std::string& current, int32_t left, std::vector<std::string>& out) {
  out.push_back(current);
  if (left == 0) return;
  for (char c : std::string_view("abc")) {
    current.push_back(c);
    strings(current, left - 1, out);
    current.pop_back();
  }
}

static Automaton randomAutomaton(std::mt19937& rng, Budget& budget, int32_t depth) {
  if (depth == 0) {
    int32_t c = 'a' + (int32_t)(rng() % 3);
    return Automaton::charRange(c, c, budget);
  }
  Automaton a = randomAutomaton(rng, budget, depth - 1);
  switch (rng() % 5) {
    case 0: return Automaton::concatenate(a, randomAutomaton(rng, budget, depth - 1), budget);
    case 1: return Automaton::unite(a, randomAutomaton(rng, budget, depth - 1), budget);
    case 2: return Automaton::optional(a, budget);
    case 3: return Automaton::star(a, budget);
    default: return Automaton::repeatRange(a, (int32_t)(rng() % 3), 2 + (int32_t)(rng() % 2), budget);
  }
}

TEST(AutomatonTest, starDoesNotAcceptPartialLoop) {
  Budget budget(1000000);
  Automaton loop;
  loop.addState(budget);
  loop.addState(budget, true);
  loop.addTransition(0, 'a', 'a', 1, budget);
  loop.addTransition(1, 'b', 'b', 0, budget);
  loop.freeze(budget);
  Automaton starred = Automaton::star(loop, budget);
  EXPECT_TRUE(accepts(starred, ""));
  EXPECT_TRUE(accepts(starred, "a"));
  EXPECT_TRUE(accepts(starred, "aba"));
  EXPECT_FALSE(accepts(starred, "ab"));
}

TEST(AutomatonTest, randomizedOperationTreeDifferential) {
  std::mt19937 rng(19840119);
  std::vector<std::string> corpus;
  std::string current;
  strings(current, 6, corpus);
  for (int32_t run = 0; run < 80; run++) {
    Budget budget(10000000);
    Automaton nfa = randomAutomaton(rng, budget, 3);
    Automaton dfa = Automaton::removeDeadStates(Automaton::determinize(nfa, budget), budget);
    for (const std::string& value : corpus) EXPECT_EQ(accepts(nfa, value), accepts(dfa, value)) << value;
  }
}

TEST(AutomatonTest, utf8ScalarBoundaries) {
  for (int32_t point : {0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff}) {
    Budget budget(1000000);
    ByteDfa dfa(utf32ToUtf8(Automaton::codepoint(point, budget), budget), budget);
    EXPECT_TRUE(dfa.matches(utf8(point))) << point;
  }
}

TEST(AutomatonTest, utf8RangeDifferential) {
  std::mt19937 rng(712367);
  const std::array<int32_t, 8> boundaries = {0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0x10ffff};
  for (int32_t run = 0; run < 100; run++) {
    int32_t lo = boundaries[(size_t)(rng() % boundaries.size())];
    int32_t hi = std::min(0x10ffff, lo + (int32_t)(rng() % 400));
    if (lo <= 0xdfff && hi >= 0xd800) hi = 0xd7ff;
    Budget budget(10000000);
    ByteDfa dfa(utf32ToUtf8(Automaton::charRange(lo, hi, budget), budget), budget);
    for (int32_t cp : {lo, hi, std::max(0, lo - 1), std::min(0x10ffff, hi + 1),
                       lo + (int32_t)(rng() % (hi - lo + 1))}) {
      if (cp >= 0xd800 && cp <= 0xdfff) continue;
      EXPECT_EQ(dfa.matches(utf8(cp)), cp >= lo && cp <= hi) << lo << ".." << hi << " cp=" << cp;
    }
  }
}

static size_t utf8Length(std::string_view value, size_t position) {
  unsigned char first = value[position];
  int32_t count = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3
                : (first & 0xf8) == 0xf0 ? 4 : 0;
  if (count == 0 || position + (size_t)count > value.size()) return 0;
  for (int32_t i = 1; i < count; i++) if (((unsigned char)value[position + (size_t)i] & 0xc0) != 0x80) return 0;
  return (size_t)count;
}

static bool wildcardReference(std::string_view pattern, std::string_view value, size_t p = 0, size_t v = 0) {
  if (p == pattern.size()) return v == value.size();
  if (pattern[p] == '*') {
    for (size_t end = v; end <= value.size(); end++) if (wildcardReference(pattern, value, p + 1, end)) return true;
    return false;
  }
  if (v == value.size()) return false;
  if (pattern[p] == '?') {
    size_t count = utf8Length(value, v);
    return count != 0 && wildcardReference(pattern, value, p + 1, v + count);
  }
  if (pattern[p] == '\\' && p + 1 < pattern.size()) return pattern[p + 1] == value[v] && wildcardReference(pattern, value, p + 2, v + 1);
  return pattern[p] == value[v] && wildcardReference(pattern, value, p + 1, v + 1);
}

TEST(WildcardTest, randomizedDifferential) {
  std::mt19937 rng(9231);
  const std::string alphabet = "abc*?\\";
  const std::vector<std::string> corpus = {"", "a", "abc", "\xc3\xa9", "\xff", "a\xff", "\x80", "ab\xc3\xa9"};
  for (int32_t run = 0; run < 150; run++) {
    std::string pattern;
    for (int32_t i = 0, n = 1 + (int32_t)(rng() % 6); i < n; i++) {
      char c = alphabet[(size_t)(rng() % alphabet.size())];
      if (c == '\\') { pattern += "\\a"; } else pattern.push_back(c);
    }
    Budget budget(10000000);
    ByteDfa dfa = compileWildcard(pattern, budget);
    for (const std::string& value : corpus) EXPECT_EQ(dfa.matches(value), wildcardReference(pattern, value)) << pattern;
  }
}

TEST(WildcardTest, compileAndClassify) {
  auto compile = [](std::string_view pattern) { Budget budget(10000000); return compileWildcard(pattern, budget); };
  std::string value;
  EXPECT_EQ(compile("*").classify(&value), ByteDfa::Kind::ALL);
  EXPECT_EQ(compile("**").classify(&value), ByteDfa::Kind::ALL);
  ByteDfa prefix = compile("foo*");
  EXPECT_EQ(prefix.classify(&value), ByteDfa::Kind::PREFIX);
  EXPECT_EQ(value, "foo");
  EXPECT_TRUE(prefix.matches(std::string("foo\xff", 4)));
  EXPECT_EQ(compile("foo*bar").classify(&value), ByteDfa::Kind::NORMAL);
  EXPECT_EQ(compile("foo\\*").classify(&value), ByteDfa::Kind::SINGLE);
  EXPECT_EQ(value, "foo*");
  EXPECT_EQ(compile(std::string(300, 'a') + "*").classify(), ByteDfa::Kind::NONE);
  Budget budget(1000000);
  EXPECT_EQ(ByteDfa(Automaton::empty(budget), budget).classify(), ByteDfa::Kind::NONE);
}

TEST(WildcardTest, errorsAreClean) {
  EXPECT_THROW({ Budget budget; compileWildcard("foo\\", budget); }, std::runtime_error);
  EXPECT_THROW({ Budget budget; compileWildcard("\xff", budget); }, std::runtime_error);
  EXPECT_THROW({ Budget budget(1); compileWildcard("abcdef", budget); }, std::runtime_error);
}

namespace {

class TestCodepointFolder final : public CodepointFolder {
public:
  int32_t fold(int32_t codepoint, int32_t* out, int32_t maxOut) const override {
    if (maxOut < 2) return -1;
    if (codepoint == 'A') { out[0] = 'a'; return 1; }
    if (codepoint == 0xdf) { out[0] = 's'; out[1] = 's'; return 2; }
    if (codepoint == 0xff0a) { out[0] = '*'; return 1; }
    if (codepoint == 'x') return 0;
    out[0] = codepoint;
    return 1;
  }
};

} // namespace

TEST(WildcardTest, literalAtomFolding) {
  TestCodepointFolder folder;
  auto compile = [&](std::string_view pattern) {
    Budget budget(10000000);
    return compileWildcard(pattern, budget, &folder);
  };
  std::string fullwidthStar = utf8(0xff0a);
  std::string value;
  EXPECT_EQ(compile(fullwidthStar).classify(&value), ByteDfa::Kind::SINGLE);
  EXPECT_EQ(value, "*");
  EXPECT_EQ(compile("*").classify(), ByteDfa::Kind::ALL);
  EXPECT_EQ(compile(utf8(0xdf) + "*").classify(&value), ByteDfa::Kind::PREFIX);
  EXPECT_EQ(value, "ss");
  EXPECT_TRUE(compile("x").matches(""));
  EXPECT_TRUE(compile("\\" + fullwidthStar).matches("*"));
}

TEST(RegExpTest, compileSemanticsAndClassification) {
  auto compile = [](std::string_view pattern) { Budget budget(10000000); return compileRegex(pattern, budget); };
  EXPECT_TRUE(compile("(a|b)*c").matches("ababc"));
  EXPECT_TRUE(compile("a{2,4}").matches("aaa"));
  EXPECT_FALSE(compile("a{2,4}").matches("a"));
  EXPECT_TRUE(compile("a{2,}").matches("aaaa"));
  EXPECT_TRUE(compile("foo.*").matches("foo"));
  EXPECT_TRUE(compile("foo.*").matches("foobar"));
  EXPECT_TRUE(compile("[b-d]").matches("c"));
  EXPECT_TRUE(compile("[^b]").matches("\xc3\xa9"));
  EXPECT_FALSE(compile("[^b]").matches("\xff"));
  EXPECT_FALSE(compile(".").matches("\xff"));
  EXPECT_TRUE(compile("x|").matches(""));
  EXPECT_TRUE(compile("|x").matches("x"));
  EXPECT_TRUE(compile("()").matches(""));
  EXPECT_TRUE(compile("((a))").matches("a"));
  EXPECT_TRUE(compile("a**").matches("aaaa"));
  EXPECT_TRUE(compile("\\(\\{\\|\\\\").matches("({|\\"));
  EXPECT_TRUE(compile("^$").matches("^$"));
  std::string value;
  EXPECT_EQ(compile("abc").classify(&value), ByteDfa::Kind::SINGLE);
  EXPECT_EQ(compile("abc.*").classify(&value), ByteDfa::Kind::NORMAL);
  EXPECT_EQ(compile("abc(d|e)").classify(&value), ByteDfa::Kind::NORMAL);
}

TEST(RegExpTest, literalAtomFolding) {
  TestCodepointFolder folder;
  Budget budget(10000000);
  ByteDfa sharpS = compileRegex(utf8(0xdf) + "{2}", budget, &folder);
  EXPECT_TRUE(sharpS.matches("ssss"));
  EXPECT_FALSE(sharpS.matches("ss"));
  EXPECT_FALSE(sharpS.matches("sssss"));
  Budget escapedBudget(10000000);
  EXPECT_TRUE(compileRegex("\\A", escapedBudget, &folder).matches("a"));
}

TEST(RegExpTest, errorsHavePositions) {
  auto fails = [](std::string_view pattern) {
    try { Budget budget; compileRegex(pattern, budget); }
    catch (const std::runtime_error& e) { return std::string(e.what()); }
    return std::string();
  };
  EXPECT_NE(fails("[").find("byte"), std::string::npos);
  EXPECT_NE(fails("(").find("byte"), std::string::npos);
  EXPECT_NE(fails("[]").find("empty"), std::string::npos);
  EXPECT_NE(fails("a{1001}").find("1000"), std::string::npos);
  EXPECT_NE(fails("}").find("must be escaped"), std::string::npos);
}

namespace {

struct ReferenceRegex {
  enum class Type { LITERAL, ANY, CLASS, NEG_CLASS, CONCAT, UNION, REPEAT };
  Type type;
  char literal = 0;
  int32_t min = 0;
  int32_t max = 0;
  std::vector<std::shared_ptr<ReferenceRegex>> children;
};

using RegexPtr = std::shared_ptr<ReferenceRegex>;

RegexPtr regexNode(ReferenceRegex::Type type) {
  auto node = std::make_shared<ReferenceRegex>();
  node->type = type;
  return node;
}

std::string regexText(const RegexPtr& node) {
  switch (node->type) {
    case ReferenceRegex::Type::LITERAL:
      return node->literal == 'a' ? "\\a" : std::string(1, node->literal);
    case ReferenceRegex::Type::ANY: return ".";
    case ReferenceRegex::Type::CLASS: return "[a-c]";
    case ReferenceRegex::Type::NEG_CLASS: return "[^b]";
    case ReferenceRegex::Type::CONCAT: {
      std::string out;
      for (const auto& child : node->children) out += regexText(child);
      return out;
    }
    case ReferenceRegex::Type::UNION:
      return "(" + regexText(node->children[0]) + "|" + regexText(node->children[1]) + ")";
    case ReferenceRegex::Type::REPEAT: {
      std::string atom = "(" + regexText(node->children[0]) + ")";
      if (node->min == 0 && node->max == -1) return atom + "*";
      if (node->min == 1 && node->max == -1) return atom + "+";
      if (node->min == 0 && node->max == 1) return atom + "?";
      if (node->max == -1) return atom + "{" + std::to_string(node->min) + ",}";
      return atom + "{" + std::to_string(node->min) + "," + std::to_string(node->max) + "}";
    }
  }
  return {};
}

RegexPtr randomRegex(std::mt19937& rng, int depth) {
  if (depth == 0) {
    switch (rng() % 4) {
      case 0: { auto n = regexNode(ReferenceRegex::Type::ANY); return n; }
      case 1: return regexNode(ReferenceRegex::Type::CLASS);
      case 2: return regexNode(ReferenceRegex::Type::NEG_CLASS);
      default: {
        auto n = regexNode(ReferenceRegex::Type::LITERAL);
        n->literal = "abc"[(size_t)(rng() % 3)];
        return n;
      }
    }
  }
  switch (rng() % 4) {
    case 0: {
      auto n = regexNode(ReferenceRegex::Type::CONCAT);
      n->children = {randomRegex(rng, depth - 1), randomRegex(rng, depth - 1)};
      return n;
    }
    case 1: {
      auto n = regexNode(ReferenceRegex::Type::UNION);
      n->children = {randomRegex(rng, depth - 1), randomRegex(rng, depth - 1)};
      return n;
    }
    default: {
      auto n = regexNode(ReferenceRegex::Type::REPEAT);
      n->children = {randomRegex(rng, depth - 1)};
      switch (rng() % 5) {
        case 0: n->min = 0; n->max = -1; break;
        case 1: n->min = 1; n->max = -1; break;
        case 2: n->min = 0; n->max = 1; break;
        case 3: n->min = 1; n->max = 3; break;
        default: n->min = 2; n->max = -1; break;
      }
      return n;
    }
  }
}

std::vector<int32_t> referenceCodepoints(std::string_view value) {
  std::vector<int32_t> out;
  for (size_t pos = 0; pos < value.size();) {
    unsigned char first = value[pos++];
    int32_t count = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2
        : (first & 0xf0) == 0xe0 ? 3 : 4;
    int32_t cp = count == 1 ? first : first & ((1 << (7 - count)) - 1);
    for (int32_t i = 1; i < count; i++) cp = (cp << 6) | ((unsigned char)value[pos++] & 0x3f);
    out.push_back(cp);
  }
  return out;
}

void addUnique(std::vector<size_t>& positions, size_t position) {
  if (std::find(positions.begin(), positions.end(), position) == positions.end()) positions.push_back(position);
}

std::vector<size_t> referenceMatch(const RegexPtr& node, const std::vector<int32_t>& text,
                                   size_t position) {
  if (node->type == ReferenceRegex::Type::LITERAL) {
    return position < text.size() && text[position] == node->literal ? std::vector<size_t>{position + 1} : std::vector<size_t>{};
  }
  if (node->type == ReferenceRegex::Type::ANY) {
    return position < text.size() ? std::vector<size_t>{position + 1} : std::vector<size_t>{};
  }
  if (node->type == ReferenceRegex::Type::CLASS || node->type == ReferenceRegex::Type::NEG_CLASS) {
    bool inClass = position < text.size() && text[position] >= 'a' && text[position] <= 'c';
    if (node->type == ReferenceRegex::Type::NEG_CLASS) inClass = position < text.size() && text[position] != 'b';
    return inClass ? std::vector<size_t>{position + 1} : std::vector<size_t>{};
  }
  if (node->type == ReferenceRegex::Type::CONCAT) {
    std::vector<size_t> positions{position};
    for (const auto& child : node->children) {
      std::vector<size_t> next;
      for (size_t current : positions) for (size_t end : referenceMatch(child, text, current)) addUnique(next, end);
      positions.swap(next);
    }
    return positions;
  }
  if (node->type == ReferenceRegex::Type::UNION) {
    std::vector<size_t> out = referenceMatch(node->children[0], text, position);
    for (size_t end : referenceMatch(node->children[1], text, position)) addUnique(out, end);
    return out;
  }
  std::vector<size_t> out;
  std::vector<size_t> frontier{position};
  int32_t limit = node->max == -1 ? std::max(node->min, (int32_t)text.size() + 1) : node->max;
  for (int32_t count = 0; count <= limit; count++) {
    if (count >= node->min) for (size_t end : frontier) addUnique(out, end);
    if (count == limit) break;
    std::vector<size_t> next;
    for (size_t current : frontier) {
      for (size_t end : referenceMatch(node->children[0], text, current)) {
        addUnique(next, end);
      }
    }
    if (next.empty()) break;
    frontier.swap(next);
  }
  return out;
}

bool referenceMatches(const RegexPtr& node, std::string_view value) {
  auto text = referenceCodepoints(value);
  auto ends = referenceMatch(node, text, 0);
  return std::find(ends.begin(), ends.end(), text.size()) != ends.end();
}

} // namespace

TEST(RegExpTest, randomizedParserDifferential) {
  std::mt19937 rng(0x5eed1234);
  std::vector<std::string> corpus;
  std::string current;
  strings(current, 5, corpus);
  corpus.insert(corpus.end(), {"\xc3\xa9", "a\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80"});
  for (int32_t run = 0; run < 100; run++) {
    RegexPtr reference = randomRegex(rng, 3);
    std::string pattern = regexText(reference);
    Budget budget(10000000);
    ByteDfa dfa = compileRegex(pattern, budget);
    for (const std::string& value : corpus) {
      EXPECT_EQ(dfa.matches(value), referenceMatches(reference, value))
          << "pattern=" << pattern << " value=" << value;
    }
  }
}

TEST(ByteDfaTest, randomizedFrozenPipelines) {
  std::mt19937 rng(12345);
  for (int32_t run = 0; run < 30; run++) {
    Budget budget(1000000);
    int32_t c = 'a' + (int32_t)(rng() % 3);
    Automaton automaton = Automaton::star(Automaton::optional(Automaton::charRange(c, c, budget), budget), budget);
    ByteDfa dfa(Automaton::removeDeadStates(Automaton::determinize(automaton, budget), budget), budget);
    for (std::string_view value : {"", "a", "b", "abc"}) (void)dfa.matches(value);
  }
}
