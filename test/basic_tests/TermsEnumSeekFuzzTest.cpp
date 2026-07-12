#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "solux/reader/FieldReader.h"
#include "solux/reader/Postings.h"
#include "solux/reader/TermsEnum.h"
#include "test/CollectionHelper.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

namespace {

bool byteLess(std::string_view a, std::string_view b) {
  uint32_t n = std::min((uint32_t)a.size(), (uint32_t)b.size());
  for (uint32_t i = 0; i < n; i++) {
    uint8_t ca = (uint8_t)a[i];
    uint8_t cb = (uint8_t)b[i];
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

std::vector<std::string> sortedUnique(std::vector<std::string> terms) {
  terms.erase(std::remove(terms.begin(), terms.end(), std::string()), terms.end());
  std::sort(terms.begin(), terms.end(), byteLess);
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  return terms;
}

std::vector<std::string>::const_iterator lowerTerm(const std::vector<std::string>& terms,
                                                   std::string_view target) {
  return std::lower_bound(terms.begin(), terms.end(), target,
      [](const std::string& term, std::string_view value) {
        return byteLess(term, value);
      });
}

bool equals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && !byteLess(a, b) && !byteLess(b, a);
}

void indexTerms(TestIndex& ti, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (int i = 0; i < (int)terms.size(); i++) {
    field.add(i, terms[(size_t)i]);
  }
  ti.flush();
  field.startReading();
  ASSERT_NE(field.currentSegment(), nullptr);
}

std::string randomAsciiTerm(std::mt19937_64& rng) {
  std::vector<std::string> prefixes = {"a", "ab", "ac", "b", "ba", "prefix", "shared"};
  std::string s = prefixes[(size_t)(rng() % prefixes.size())];
  int suffixLen = 1 + (int)(rng() % 6);
  for (int i = 0; i < suffixLen; i++) {
    s.push_back((char)('a' + (rng() % 26)));
  }
  return s;
}

void addTargetVariants(std::vector<std::string>& targets, const std::string& term) {
  targets.push_back(term);
  targets.push_back(term + '\0');
  targets.push_back(term + '~');
  for (uint32_t len = 0; len <= std::min(3u, (uint32_t)term.size()); len++) {
    targets.push_back(term.substr(0, len));
  }
  if (!term.empty()) {
    uint8_t last = (uint8_t)term.back();
    if (last > 'a') {
      std::string t = term;
      t.back() = (char)(last - 1);
      targets.push_back(t);
    }
    if (last < 'z') {
      std::string t = term;
      t.back() = (char)(last + 1);
      targets.push_back(t);
    }
  }
}

std::vector<std::string> makeTerms(std::mt19937_64& rng) {
  std::vector<std::string> terms;
  for (int i = 0; i < 220; i++) {
    terms.push_back(randomAsciiTerm(rng));
  }
  for (int i = 0; i < 80; i++) {
    terms.push_back("cluster" + std::to_string(i / 10) + "_" + std::to_string(i));
  }
  return sortedUnique(std::move(terms));
}

std::vector<std::string> makeTargets(std::mt19937_64& rng, const std::vector<std::string>& terms) {
  std::vector<std::string> targets;
  targets.emplace_back();
  for (const std::string& term : terms) {
    addTargetVariants(targets, term);
  }
  for (int i = 0; i < 120; i++) {
    targets.push_back(randomAsciiTerm(rng));
  }
  return targets;
}

SegFieldInfo readFieldInfo(MemPool& pool, PostingsReader& postingsReader, std::string_view fieldName) {
  FieldReader fieldReader(pool, postingsReader);
  EXPECT_TRUE(fieldReader.seek(fieldName));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  return fieldInfo;
}

void expectSeekAndSeekCeil(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo,
                           const std::vector<std::string>& terms,
                           const std::vector<std::string>& targets) {
  for (const std::string& target : targets) {
    MemPool pool;
    auto it = lowerTerm(terms, target);
    bool exact = it != terms.end() && equals(*it, target);

    TermsEnum seekEnum(pool, postingsReader, fieldInfo);
    EXPECT_EQ(seekEnum.seek(target), exact) << target;
    if (exact) {
      EXPECT_EQ((std::string_view)seekEnum.term(), *it) << target;
    }

    TermsEnum ceilEnum(pool, postingsReader, fieldInfo);
    bool gotCeil = ceilEnum.seekCeil(target);
    EXPECT_EQ(gotCeil, it != terms.end()) << target;
    if (it != terms.end()) {
      EXPECT_EQ((std::string_view)ceilEnum.term(), *it) << target;
    }
  }
}

void expectSeekForward(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo,
                       const std::vector<std::string>& terms) {
  MemPool pool;
  TermsEnum te(pool, postingsReader, fieldInfo);

  for (size_t i = 0; i < terms.size(); i++) {
    const std::string& term = terms[i];
    ASSERT_TRUE(te.seekForward(term)) << term;
    EXPECT_EQ((std::string_view)te.term(), term);

    std::string afterTerm = term + '\0';
    bool exact = te.seekForward(afterTerm);
    EXPECT_FALSE(exact) << afterTerm;
    if (i + 1 < terms.size()
        && (int32_t)i / Postings::TERMS_BLOCK_SIZE == (int32_t)(i + 1) / Postings::TERMS_BLOCK_SIZE) {
      EXPECT_EQ((std::string_view)te.term(), terms[i + 1]) << afterTerm;
    } else {
      if (i + 1 == terms.size()) break;
    }
  }
}

} // namespace

class TermsEnumSeekFuzzTest : public SoluxTest {
};

TEST_F(TermsEnumSeekFuzzTest, GapRoutingAcrossSeparatorBoundary) {
  // A target after block 0's last term but before block 1's first term is a
  // miss, and seekCeil must still land on block 1's first term.
  TestIndex ti;
  TestField field(ti, "gap_s");
  std::vector<std::string> terms;
  for (int i = 0; i < Postings::TERMS_BLOCK_SIZE; i++) {
    terms.push_back("a" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i));
  }
  terms.push_back("azzz");
  terms = sortedUnique(std::move(terms));
  ASSERT_EQ(terms[(size_t)Postings::TERMS_BLOCK_SIZE - 1], "a031");
  ASSERT_EQ(terms[(size_t)Postings::TERMS_BLOCK_SIZE], "azzz");

  indexTerms(ti, field, terms);

  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum seekEnum(guard.pool(), seg->postingsReader(), field.fieldInfo);
  ASSERT_FALSE(seekEnum.seek("azm"));
  EXPECT_EQ((std::string_view)seekEnum.term(), "azzz");

  TermsEnum ceilEnum(guard.pool(), seg->postingsReader(), field.fieldInfo);
  ASSERT_TRUE(ceilEnum.seekCeil("azm"));
  EXPECT_EQ((std::string_view)ceilEnum.term(), "azzz");
}

TEST_F(TermsEnumSeekFuzzTest, RandomCollectionTermsMatchVectorReference) {
  std::mt19937_64 rng(0x5eedf00d);
  CollectionHelper helper;

  std::vector<std::string> terms = makeTerms(rng);
  ASSERT_GT(terms.size(), (size_t)Postings::TERMS_BLOCK_SIZE * 3);

  std::vector<Doc> docs;
  docs.reserve(terms.size());
  for (size_t i = 0; i < terms.size(); i++) {
    docs.push_back({
        {"id", std::string("tef_") + std::to_string(i)},
        {"seek_s", terms[i]}
    });
  }
  IndexResult result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(result.success) << result.error_message;

  auto reader = helper.getIndexWriter()->getIndexReader(0);
  ASSERT_EQ(reader->segments().size(), 1u);
  Segment& seg = reader->segments()[0];

  MemPool pool;
  SegFieldInfo fieldInfo = readFieldInfo(pool, seg.postingsReader(), "seek_s");
  std::vector<std::string> targets = makeTargets(rng, terms);

  expectSeekAndSeekCeil(seg.postingsReader(), fieldInfo, terms, targets);
  expectSeekForward(seg.postingsReader(), fieldInfo, terms);
}
