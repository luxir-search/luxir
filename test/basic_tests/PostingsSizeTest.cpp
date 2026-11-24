// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Docs-file size breakdown on a small synthetic corpus: a Zipf-like text field
// with positions, a keyword field dominated by low-docFreq terms and a unique
// id field.  The test prints the per-field breakdown and the segment size, so
// a postings format change can report its effect, and checks the invariants
// that the format promises for each postings shape.

#include "gtest/gtest.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/random.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace luxir;
using namespace luxir::test;

namespace {

class PostingsSizeTest : public LuxirTest {};

constexpr int32_t NUM_DOCS = 20000;
constexpr int32_t VOCABULARY = 40000;
constexpr int32_t TAG_VALUES = 30000;

std::shared_ptr<Schema> corpusSchema() {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");
  schema->fieldTypeMap["tag"] = std::make_shared<StrFieldType>("tag", FieldType::INDEX_DOCS);
  schema->fieldTypeMap["id"] = std::make_shared<IdFieldType>("id");
  return schema;
}

// Inverse-CDF sampler over ranks 0..n-1 with P(rank) ~ 1 / (rank + 1)^s.
class Zipf {
  std::vector<double> cdf;

public:
  Zipf(int32_t n, double s) : cdf((size_t) n) {
    double sum = 0;
    for (int32_t i = 0; i < n; i++) {
      sum += 1.0 / std::pow((double) i + 1, s);
      cdf[(size_t) i] = sum;
    }
    for (double& c : cdf) c /= sum;
  }

  int32_t operator()(Rng& rng) const {
    double u = (double) (rng() >> 11) * 0x1.0p-53;
    return (int32_t) (std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
  }
};

void printBreakdown(std::string_view field, const TextWriter::DocsFileBytes& b) {
  std::printf("%-5s docs=%8llu  l1=%7llu l0=%7llu fullDocs=%7llu fullFreqs=%7llu"
              " tailDocs=%7llu tailFreqs=%7llu | terms pulsed=%llu tailOnly=%llu blocks=%llu\n",
              std::string(field).c_str(), (unsigned long long) b.total(),
              (unsigned long long) b.l1Headers, (unsigned long long) b.l0Headers,
              (unsigned long long) b.fullDocs, (unsigned long long) b.fullFreqs,
              (unsigned long long) b.tailDocs, (unsigned long long) b.tailFreqs,
              (unsigned long long) b.pulsedTerms, (unsigned long long) b.tailOnlyTerms,
              (unsigned long long) b.blockTerms);
}

}  // namespace

TEST_F(PostingsSizeTest, docsFileBreakdown) {
  TestIndex index;
  test::reserveIncarnation(index.dir);
  index.iw = std::make_unique<IndexWriter>(index.snapshots, corpusSchema());
  TestField body(index, "body");
  TestField tag(index, "tag");
  TestField id(index, "id");

  Rng rng(42);
  Zipf words(VOCABULARY, 1.07);
  Zipf tags(TAG_VALUES, 0.6);
  TextWriter::clearDocsFileBytes();
  TextWriter::collectDocsFileBytes.store(true, std::memory_order_relaxed);
  body.startIndexing();
  tag.startIndexing();
  id.startIndexing();
  std::string text;
  for (int32_t doc = 0; doc < NUM_DOCS; doc++) {
    text.clear();
    int32_t len = 20 + rng.rint(200);
    for (int32_t i = 0; i < len; i++) {
      if (i > 0) text.push_back(' ');
      text += "w" + std::to_string(words(rng));
    }
    body.add(doc, text);
    tag.add(doc, "t" + std::to_string(tags(rng)));
    id.add(doc, "doc" + std::to_string(doc));
  }
  index.flush();
  TextWriter::collectDocsFileBytes.store(false, std::memory_order_relaxed);
  index.initReader();
  ASSERT_EQ(1u, index.reader->segments().size());

  auto byField = TextWriter::docsFileBytesByField();
  ASSERT_EQ(3u, byField.size());
  uint64_t docsTotal = 0;
  for (const char* field : {"body", "id", "tag"}) {
    const auto& bytes = byField.at(field);
    printBreakdown(field, bytes);
    docsTotal += bytes.total();
  }
  uint64_t segmentBytes = index.reader->segments()[0].postingsReader().core().sizeInBytes();
  std::printf("docs files total=%llu segment total=%llu\n",
              (unsigned long long) docsTotal, (unsigned long long) segmentBytes);

  // Every id has one doc and is pulsed; docs-only fields store no freqs.
  const auto& ids = byField.at("id");
  EXPECT_EQ((uint64_t) NUM_DOCS, ids.pulsedTerms);
  EXPECT_EQ(0u, ids.total());
  EXPECT_EQ(0u, byField.at("tag").fullFreqs + byField.at("tag").tailFreqs);
  EXPECT_GT(byField.at("tag").tailOnlyTerms, 0u);
  EXPECT_GT(byField.at("body").blockTerms, 0u);
  // A term without a full block writes no headers, so only terms with full
  // blocks pay L1 and L0 headers: at least one of each per such term.
  for (const auto& [field, bytes] : byField) {
    EXPECT_GE(bytes.l0Headers, bytes.blockTerms) << field;
    EXPECT_LE(bytes.l1Headers, bytes.blockTerms * 64) << field;
  }
  EXPECT_LT(byField.at("tag").l1Headers + byField.at("tag").l0Headers, 200u);
}
