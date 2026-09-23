// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <atomic>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "luxir/index/IndexRamBudget.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/MergeCostModel.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/Directory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"

using namespace luxir;
using IndexMode = luxir::api::FieldDef::IndexMode;

class StoredFieldsTest : public ::testing::Test {
protected:
  static std::shared_ptr<Schema> makeSchema() {
    auto s = std::make_shared<Schema>();
    s->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
        "body",
        FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
        "whitespace");
    s->fieldTypeMap["title"] = std::make_shared<TextFieldType>(
        "title",
        FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
        "whitespace");
    s->fieldTypeMap["tags"] = std::make_shared<TextFieldType>(
        "tags",
        FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::MULTI_VALUED | FieldType::STORED,
        "whitespace");
    return s;
  }

  // Collected stored values for a doc, grouped by field name in first-seen order.
  // vals is (fieldName, listOfValues).
  struct StoredDoc {
    std::vector<std::pair<std::string, std::vector<std::string>>> vals;

    // Flatten to (fieldName, firstValue) - convenience for single-valued checks.
    std::vector<std::pair<std::string, std::string>> flat() const {
      std::vector<std::pair<std::string, std::string>> out;
      for (const auto& [name, values] : vals) {
        for (const auto& v : values) {
          out.emplace_back(name, v);
        }
      }
      return out;
    }
  };

  static StoredDoc readStored(StoredFieldsReader& r, int32_t docID) {
    StoredDoc out;
    r.readDoc(docID,
        [&](std::string_view n, std::span<const std::string_view> values) {
      auto& entry = out.vals.emplace_back();
      entry.first = std::string(n);
      entry.second.reserve(values.size());
      for (auto v : values) entry.second.emplace_back(v);
    });
    return out;
  }

  static std::string bigStoredValue() {
    std::string big(40 * 1024, 'x');  // 40KB
    // Spaces keep individual tokens under the indexed-term length limit; the
    // stored side still sees one 40KB value.
    for (size_t i = 0; i < big.size(); i++) {
      big[i] = (i % 100 == 99) ? ' ' : (char)('a' + (i % 26));
    }
    return big;
  }
};

TEST_F(StoredFieldsTest, basicSingleValued) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("hello world"));
    inv.getIndexHandler("title").index(inv, std::string_view("greeting"));
    inv.finishDoc();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("goodbye"));
    inv.finishDoc();

    inv.startDoc();
    // doc 2 has no stored fields
    inv.finishDoc();

    inv.startDoc();
    inv.getIndexHandler("title").index(inv, std::string_view("farewell"));
    inv.finishDoc();

    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto& seg = reader->segments()[0];
  MemPool pool;
  auto sfr = StoredFieldsReader::open(seg.postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_EQ(sfr->maxDoc(), 4);

  auto d0 = readStored(*sfr, 0).flat();
  ASSERT_EQ(d0.size(), 2u);
  EXPECT_EQ(d0[0], (std::pair<std::string, std::string>{"body", "hello world"}));
  EXPECT_EQ(d0[1], (std::pair<std::string, std::string>{"title", "greeting"}));

  auto d1 = readStored(*sfr, 1).flat();
  ASSERT_EQ(d1.size(), 1u);
  EXPECT_EQ(d1[0], (std::pair<std::string, std::string>{"body", "goodbye"}));

  auto d2 = readStored(*sfr, 2);
  EXPECT_EQ(d2.vals.size(), 0u);

  auto d3 = readStored(*sfr, 3).flat();
  ASSERT_EQ(d3.size(), 1u);
  EXPECT_EQ(d3[0], (std::pair<std::string, std::string>{"title", "farewell"}));
}

TEST_F(StoredFieldsTest, multiValued) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();

    std::vector<std::string_view> tags1 = {"red", "blue", "green"};
    inv.startDoc();
    inv.getIndexHandler("tags").index(inv, std::span<std::string_view>(tags1));
    inv.finishDoc();

    std::vector<std::string_view> tags2 = {"solo"};
    inv.startDoc();
    inv.getIndexHandler("tags").index(inv, std::span<std::string_view>(tags2));
    inv.finishDoc();

    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);

  auto d0 = readStored(*sfr, 0);
  ASSERT_EQ(d0.vals.size(), 1u);  // one grouped field callback
  EXPECT_EQ(d0.vals[0].first, "tags");
  ASSERT_EQ(d0.vals[0].second.size(), 3u);
  EXPECT_EQ(d0.vals[0].second[0], "red");
  EXPECT_EQ(d0.vals[0].second[1], "blue");
  EXPECT_EQ(d0.vals[0].second[2], "green");

  auto d1 = readStored(*sfr, 1);
  ASSERT_EQ(d1.vals.size(), 1u);
  EXPECT_EQ(d1.vals[0].first, "tags");
  ASSERT_EQ(d1.vals[0].second.size(), 1u);
  EXPECT_EQ(d1.vals[0].second[0], "solo");
}

TEST_F(StoredFieldsTest, manyDocsMultipleChunks) {
  // 400 docs will cross the 128-doc-per-chunk boundary three times, ensuring
  // interpolation across chunks works.
  constexpr int32_t N = 400;
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();
    for (int32_t i = 0; i < N; i++) {
      inv.startDoc();
      std::string body = "doc number " + std::to_string(i);
      inv.getIndexHandler("body").index(inv, std::string_view(body));
      inv.finishDoc();
    }
    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_EQ(sfr->maxDoc(), N);
  EXPECT_GE(sfr->numChunks(), 3);  // at least a few chunks

  // Spot-check every 37th doc, and the edges.
  std::vector<int32_t> probes;
  for (int32_t i = 0; i < N; i += 37) probes.push_back(i);
  probes.push_back(0);
  probes.push_back(127);
  probes.push_back(128);  // chunk boundary
  probes.push_back(N - 1);
  for (int32_t i : probes) {
    auto d = readStored(*sfr, i).flat();
    ASSERT_EQ(d.size(), 1u) << " at doc " << i;
    EXPECT_EQ(d[0], (std::pair<std::string, std::string>{"body", "doc number " + std::to_string(i)}));
  }
}

TEST_F(StoredFieldsTest, oversizeDoc) {
  // A single doc with more than 16KB of stored text must fit in its own chunk.
  RAMDir dir;
  auto schema = makeSchema();
  std::string big = bigStoredValue();

  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view(big));
    inv.finishDoc();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("small"));
    inv.finishDoc();

    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_GE(sfr->maxChunkBytes(), (int64_t)big.size());
  auto d0 = readStored(*sfr, 0).flat();
  ASSERT_EQ(d0.size(), 1u);
  EXPECT_EQ(d0[0].first, "body");
  EXPECT_EQ(d0[0].second, big);
  auto d1 = readStored(*sfr, 1).flat();
  ASSERT_EQ(d1.size(), 1u);
  EXPECT_EQ(d1[0], (std::pair<std::string, std::string>{"body", "small"}));
}

TEST_F(StoredFieldsTest, segmentMerge) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    // Segment 1
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("alpha"));
      inv.finishDoc();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("beta"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    // Segment 2
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("gamma"));
      inv.finishDoc();
      inv.startDoc();
      inv.getIndexHandler("title").index(inv, std::string_view("delta title"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }

    ASSERT_EQ(iw.snapshots.readers.getReader()->segments().size(), 2u);

    iw.mergeSegments();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_EQ(sfr->maxDoc(), 4);

  EXPECT_EQ(readStored(*sfr, 0).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "alpha"}}));
  EXPECT_EQ(readStored(*sfr, 1).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "beta"}}));
  EXPECT_EQ(readStored(*sfr, 2).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "gamma"}}));
  EXPECT_EQ(readStored(*sfr, 3).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"title", "delta title"}}));
}

TEST_F(StoredFieldsTest, oversizeDocMaxChunkBytesRoundTripsThroughMerge) {
  RAMDir dir;
  auto schema = makeSchema();
  std::string big = bigStoredValue();
  IndexRamBudget budget(MergeCostModel::LIGHT_BYTES + 128);
  std::atomic<int64_t> maxReserved{0};

  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema, &budget);

    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view(big));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("small"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }

    luxir::Signal::listen("segmentMergeBody",
        [&budget, &maxReserved](void*, void*, void*) -> void* {
          int64_t reserved = budget.reservedBytes();
          int64_t prev = maxReserved.load(std::memory_order_relaxed);
          while (prev < reserved
                 && !maxReserved.compare_exchange_weak(
                     prev, reserved, std::memory_order_relaxed)) {
          }
          return nullptr;
        });
    auto cleanup = luxir::scope_guard([]() {
      luxir::Signal::unlisten("segmentMergeBody");
    });

    iw.mergeSegments();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_GE(sfr->maxChunkBytes(), (int64_t)big.size());
  EXPECT_GT(maxReserved.load(std::memory_order_relaxed),
            MergeCostModel::LIGHT_BYTES + (int64_t)big.size());
}

TEST_F(StoredFieldsTest, mergeOneSegmentHasNoStored) {
  // One segment has stored fields, the other doesn't.  Merger must still pad
  // correctly.
  RAMDir dir;
  auto schemaWithStored = makeSchema();
  auto schemaPlain = std::make_shared<Schema>();
  schemaPlain->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");

  // Build segment with stored fields using schemaWithStored, and segment
  // without stored fields using schemaPlain, by swapping schemas between
  // flushes.
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schemaWithStored);

    // Segment 1: stored on
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("stored_a"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }

    // Segment 2: stored off
    iw.setSchema(schemaPlain);
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("plain_a"));
      inv.finishDoc();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("plain_b"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }

    iw.setSchema(schemaWithStored);
    iw.mergeSegments();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_EQ(sfr->maxDoc(), 3);

  EXPECT_EQ(readStored(*sfr, 0).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "stored_a"}}));
  EXPECT_EQ(readStored(*sfr, 1).vals.size(), 0u);
  EXPECT_EQ(readStored(*sfr, 2).vals.size(), 0u);
}

// Merge preserves multi-valued grouping: a 3-value "tags" field from a source
// segment must come back as a single grouped callback, not three separate ones.
TEST_F(StoredFieldsTest, mergePreservesMultiValuedGrouping) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    {
      auto& inv = iw.obtainInverter();
      std::vector<std::string_view> tagsA = {"x", "y", "z"};
      inv.startDoc();
      inv.getIndexHandler("tags").index(inv, std::span<std::string_view>(tagsA));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    {
      auto& inv = iw.obtainInverter();
      std::vector<std::string_view> tagsB = {"p", "q"};
      inv.startDoc();
      inv.getIndexHandler("tags").index(inv, std::span<std::string_view>(tagsB));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    iw.mergeSegments();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);

  auto d0 = readStored(*sfr, 0);
  ASSERT_EQ(d0.vals.size(), 1u);
  EXPECT_EQ(d0.vals[0].first, "tags");
  EXPECT_EQ(d0.vals[0].second, (std::vector<std::string>{"x", "y", "z"}));

  auto d1 = readStored(*sfr, 1);
  ASSERT_EQ(d1.vals.size(), 1u);
  EXPECT_EQ(d1.vals[0].first, "tags");
  EXPECT_EQ(d1.vals[0].second, (std::vector<std::string>{"p", "q"}));
}

// A source segment whose field-id table conflicts with the output's cannot be
// chunk-copied (compressed bodies reference the ids) and must fall back to the
// doc-by-doc re-add.  Segment A sees body first, segment B sees title first,
// segment C matches A again - so the merge runs copy, re-add, copy, which also
// exercises sealing the re-add path's partial chunk before a raw append.
TEST_F(StoredFieldsTest, mergeFieldTableMismatchFallsBack) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("a-body"));
      inv.getIndexHandler("title").index(inv, std::string_view("a-title"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("title").index(inv, std::string_view("b-title"));
      inv.getIndexHandler("body").index(inv, std::string_view("b-body"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("c-body"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }
    ASSERT_EQ(iw.snapshots.readers.getReader()->segments().size(), 3u);
    iw.mergeSegments();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_EQ(sfr->maxDoc(), 3);
  EXPECT_EQ(readStored(*sfr, 0).flat(),
            (std::vector<std::pair<std::string, std::string>>{
                {"body", "a-body"}, {"title", "a-title"}}));
  EXPECT_EQ(readStored(*sfr, 1).flat(),
            (std::vector<std::pair<std::string, std::string>>{
                {"title", "b-title"}, {"body", "b-body"}}));
  EXPECT_EQ(readStored(*sfr, 2).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "c-body"}}));
}

// Two TEXT fields routing to two different stored-fields resources (column
// families) in the same segment.  Each resource writes its own chunks and
// its own doc->chunk columns.
TEST_F(StoredFieldsTest, columnFamilies) {
  RAMDir dir;

  auto schema = std::make_shared<Schema>();
  // Register two stored-fields resources.  The default "_stored_" would
  // normally be added by createDefaultSchema, but we build a blank schema
  // here so declare it explicitly.
  schema->fieldTypeMap["_stored_"] = std::make_shared<StoredFieldType>("_stored_");
  schema->fieldTypeMap["_stored_embeddings_"] =
      std::make_shared<StoredFieldType>("_stored_embeddings_");

  // body -> default resource; paragraphs -> _stored_embeddings_
  auto bodyFt = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
      "whitespace");
  auto paraFt = std::make_shared<TextFieldType>(
      "paragraphs", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
      "whitespace");
  paraFt->storedResource_ = "_stored_embeddings_";
  schema->fieldTypeMap["body"] = bodyFt;
  schema->fieldTypeMap["paragraphs"] = paraFt;

  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("hello"));
    inv.getIndexHandler("paragraphs").index(inv, std::string_view("paragraph one"));
    inv.finishDoc();

    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("world"));
    inv.finishDoc();

    inv.startDoc();
    inv.getIndexHandler("paragraphs").index(inv, std::string_view("paragraph three"));
    inv.finishDoc();

    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto& seg = reader->segments()[0];

  MemPool pool;
  auto defaultReader = StoredFieldsReader::open(seg.postingsReader());
  ASSERT_NE(defaultReader, nullptr);
  auto paraReader = StoredFieldsReader::open(seg.postingsReader(), "_stored_embeddings_");
  ASSERT_NE(paraReader, nullptr);

  // Each resource sees all docs (including empties) and only its own fields.
  EXPECT_EQ(readStored(*defaultReader, 0).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "hello"}}));
  EXPECT_EQ(readStored(*defaultReader, 1).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"body", "world"}}));
  EXPECT_EQ(readStored(*defaultReader, 2).flat().size(), 0u);

  EXPECT_EQ(readStored(*paraReader, 0).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"paragraphs", "paragraph one"}}));
  EXPECT_EQ(readStored(*paraReader, 1).flat().size(), 0u);
  EXPECT_EQ(readStored(*paraReader, 2).flat(),
            (std::vector<std::pair<std::string, std::string>>{{"paragraphs", "paragraph three"}}));
}

// Exercise the varint field-id path: >127 stored fields in a single doc so
// that field ids cross the single-byte vint boundary.  Uses a schema with a
// "_t" abstract text suffix so we can invent many field names on the fly.
TEST_F(StoredFieldsTest, manyFieldsVarintBoundary) {
  constexpr int N_FIELDS = 200;
  RAMDir dir;
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["_stored_"] = std::make_shared<StoredFieldType>("_stored_");
  for (int i = 0; i < N_FIELDS; i++) {
    auto name = "f" + std::to_string(i);
    schema->fieldTypeMap[name] = std::make_shared<TextFieldType>(
        name, FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
        "whitespace");
  }

  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();
    inv.startDoc();
    for (int i = 0; i < N_FIELDS; i++) {
      auto name = "f" + std::to_string(i);
      auto val = "v" + std::to_string(i);
      inv.getIndexHandler(name).index(inv, std::string_view(val));
    }
    inv.finishDoc();
    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  auto d0 = readStored(*sfr, 0).flat();
  ASSERT_EQ(d0.size(), (size_t)N_FIELDS);
  // Field order is the order handlers were first created.  Handler-sort is
  // alphabetical, so "f0", "f1", ... in lexicographic order.  We don't
  // assert that exact ordering - just that every (fN, vN) pair is present.
  std::set<std::pair<std::string, std::string>> got(d0.begin(), d0.end());
  ASSERT_EQ(got.size(), (size_t)N_FIELDS);
  for (int i = 0; i < N_FIELDS; i++) {
    EXPECT_TRUE(got.contains({"f" + std::to_string(i), "v" + std::to_string(i)}))
        << " missing f" << i;
  }
}

TEST_F(StoredFieldsTest, emptySegment) {
  // A segment with no stored-field values registered should not produce a
  // stored-fields resource, and open() should return nullptr.
  RAMDir dir;
  auto schema = std::make_shared<Schema>();
  // Only a plain (non-STORED) text field.
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");

  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, schema);
    auto& inv = iw.obtainInverter();
    inv.startDoc();
    inv.getIndexHandler("body").index(inv, std::string_view("not stored"));
    inv.finishDoc();
    iw.releaseInverter(inv, true);
    iw.commit();
  }

  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  MemPool pool;
  auto sfr = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  EXPECT_EQ(sfr, nullptr);
}


// End-to-end: a STORED TEXT field should come back in search results when
// requested via TopDocs.fields.
class StoredFieldsSearchTest : public luxir::LuxirTest {};

// A segment with deletions cannot be chunk-copied (doc ids compact); the merge
// re-adds its live docs and the deleted doc's values must not survive.
TEST_F(StoredFieldsSearchTest, mergeWithDeletesDropsDeletedStored) {
  using namespace luxir::test;
  CollectionHelper ch;

  ch.index(flatdoc("id", std::string("d1"), "body_t", std::string("first")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d2"), "body_t", std::string("second")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d3"), "body_t", std::string("third")),
           UpdateMessage::COMMIT);
  // Delete d2, then force-merge to one segment: the merge sees deletions.
  ASSERT_TRUE(ch.deleteById("d2", UpdateMessage::COMMIT, 1).success);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "body_t"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"),
                                        "body_t", std::string("first"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d3"),
                                        "body_t", std::string("third"))));
}


TEST_F(StoredFieldsSearchTest, returnsStoredTextInSearch) {
  using namespace luxir::test;

  CollectionHelper ch;

  // Start from the default schema (which has "id" and the "_stored_" resource)
  // and add two explicit TEXT fields with STORED set: "body" single-valued and
  // "tags" multi-valued.  This variant bypasses proto and installs FieldTypes
  // directly - complements the round-trip-through-proto coverage elsewhere.
  auto schema = Schema::createDefaultSchema();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body",
      FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
      "whitespace");
  schema->fieldTypeMap["tags"] = std::make_shared<TextFieldType>(
      "tags",
      FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::MULTI_VALUED | FieldType::STORED,
      "whitespace");
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("d1"),
                   "body", std::string("hello world"),
                   "tags", vecs("red", "blue")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d2"),
                   "body", std::string("second doc body")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d3"),
                   "body", std::string("third"),
                   "tags", vecs("solo")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "body", "tags"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(3, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"),
                                        "body", std::string("hello world"),
                                        "tags", vecs("red", "blue"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d2"),
                                        "body", std::string("second doc body"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d3"),
                                        "body", std::string("third"),
                                        "tags", vecs("solo"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// The default `_t` dynamic suffix should produce a TEXT field that is
// STORED, so values come back in search results out of the box with no
// schema customization.
TEST_F(StoredFieldsSearchTest, defaultTSuffixIsStored) {
  using namespace luxir::test;

  CollectionHelper ch;
  // Ensure we're on the default schema.
  ch.collection().setSchema(Schema::createDefaultSchema());

  ch.index(flatdoc("id", std::string("a"),
                   "body_t", std::string("The Quick Brown Fox")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("b"),
                   "body_t", std::string("Lazy Dog")),
           UpdateMessage::COMMIT);

  {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q")
        .allQuery()
        .fields({"id", "body_t"})
        .limit(-1);
    req->execute();
    ASSERT_OK(req);
    auto docs = req->getDocs();

    ASSERT_EQ(2, docs.size());
    // Raw (not lowercased) values come back - the _t analyzer lowercases for
    // indexing/search, but stored fields preserve the original bytes.
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("a"),
                                          "body_t", std::string("The Quick Brown Fox"))));
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("b"),
                                          "body_t", std::string("Lazy Dog"))));
  }

  // And search against the tokenized (lowercased) form still works.
  {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q")
        .matchQuery("body_t", "quick")
        .fields({"id", "body_t"})
        .limit(-1);
    req->execute();
    ASSERT_OK(req);
    auto matches = req->getDocs();
    ASSERT_EQ(1, matches.size());
    EXPECT_TRUE(containsDoc(matches, flatdoc("id", std::string("a"),
                                             "body_t", std::string("The Quick Brown Fox"))));
  }

}

// A custom schema can mark a STRING field STORED to get raw-value retrieval
// through the stored-fields resource.  Verify single-valued and multi-valued
// work end-to-end.
TEST_F(StoredFieldsSearchTest, storedStringField) {
  using namespace luxir::test;

  CollectionHelper ch;

  auto schema = Schema::createDefaultSchema();
  // Indexed STRING fields (no column) with STORED set so retrieval goes
  // through the stored-fields resource rather than the ord column.
  {
    SchemaBuilder b;
    auto& f = b.field("label");
    f.type = luxir::api::FieldDef::FieldClass::STRING;
    f.index = IndexMode::MATCH;
    f.column = false;
    f.stored = true;
    auto& f2 = b.field("aliases");
    f2.type = luxir::api::FieldDef::FieldClass::STRING;
    f2.index = IndexMode::MATCH;
    f2.column = false;
    f2.multi = true;
    f2.stored = true;
    schema = b.build(schema.get());
  }
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("s1"),
                   "label", std::string("Hello World"),
                   "aliases", vecs("hi", "hola", "hey")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("s2"),
                   "label", std::string("Second")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "label", "aliases"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("s1"),
                                        "label", std::string("Hello World"),
                                        "aliases", vecs("hi", "hola", "hey"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("s2"),
                                        "label", std::string("Second"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// A STRING field that is BOTH column-stored and STORED retrieves through the
// column (faster; no LZ4 decompression).  The stored-fields copy is written
// but unused for retrieval - it's only consulted when there is no column.
TEST_F(StoredFieldsSearchTest, columnPreferredOverStored) {
  using namespace luxir::test;

  CollectionHelper ch;

  auto schema = Schema::createDefaultSchema();
  // Column-only (not indexed) STRING that is also STORED.
  SchemaBuilder b;
  auto& f = b.field("tag");
  f.type = luxir::api::FieldDef::FieldClass::STRING;
  f.index = IndexMode::NONE;
  f.column = true;
  f.stored = true;
  schema = b.build(schema.get());
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("d1"), "tag", std::string("red")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d2"), "tag", std::string("blue")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "tag"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"), "tag", std::string("red"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d2"), "tag", std::string("blue"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// stored_resource on FieldDef routes a TEXT field to a named column family.
// Two fields in two different resources should retrieve independently -
// different chunks, different decompression - and the right values land
// in the response.
TEST_F(StoredFieldsSearchTest, customStoredResourceFromProto) {
  using namespace luxir::test;

  CollectionHelper ch;

  // Build a schema with two stored-fields resources and two TEXT fields
  // routed into them.  The "_stored_" default resource is auto-added by
  // fromProto; the named resource must be registered explicitly.
  auto schema = Schema::createDefaultSchema();
  schema->fieldTypeMap["_stored_embeddings_"] =
      std::make_shared<StoredFieldType>("_stored_embeddings_");
  SchemaBuilder b;
  auto& body = b.field("body");
  body.type = luxir::api::FieldDef::FieldClass::TEXT;
  body.index = IndexMode::MATCH;
  body.stored = true;
  auto& para = b.field("paragraphs");
  para.type = luxir::api::FieldDef::FieldClass::TEXT;
  para.index = IndexMode::MATCH;
  para.stored = true;
  para.stored_resource = "_stored_embeddings_";
  schema = b.build(schema.get());
  ASSERT_EQ("_stored_embeddings_", schema->getFieldTypePtr("paragraphs")->storedResource_);
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("d1"),
                   "body", std::string("default resource"),
                   "paragraphs", std::string("custom resource")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "body", "paragraphs"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(1, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"),
                                        "body", std::string("default resource"),
                                        "paragraphs", std::string("custom resource"))));

  // The default projection discovers fields in every stored resource, named
  // families included.
  auto req2 = localReq(ch.getSearchEngine());
  req2->collection("main").topDocs("q").allQuery().limit(-1);
  req2->execute();
  ASSERT_OK(req2);
  EXPECT_TRUE(containsDoc(req2->getDocs(), flatdoc("id", std::string("d1"),
                                                   "body", std::string("default resource"),
                                                   "paragraphs", std::string("custom resource"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// ID fields accept the STORED flag and round-trip through the stored-fields
// resource just like STRING/TEXT.  Users who care about co-locating the id
// with other stored fields in the same chunk (one decompression per doc) may
// opt in; others can leave STORED off and rely on column retrieval.
TEST_F(StoredFieldsSearchTest, storedIdField) {
  using namespace luxir::test;

  CollectionHelper ch;

  auto schema = Schema::createDefaultSchema();
  schema->fieldTypeMap["id"] = std::make_shared<IdFieldType>(
      "id", FieldType::INDEX_DOCS | FieldType::COLUMN_STORED | FieldType::STORED);
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("xyz")), UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("abc")), UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("xyz"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("abc"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// Silent-data-loss regression: a STRING field with a column written in an
// older segment, plus newer segments written after STORED was enabled on
// that field.  Retrieval should pull from stored fields for new segments
// and fall back to the column for old ones - no empty slots.
TEST_F(StoredFieldsSearchTest, preStoredSegmentFallbackToColumn) {
  using namespace luxir::test;

  CollectionHelper ch;

  // Segment 1: schema has the STRING field WITHOUT stored - only column.
  {
    auto schema = Schema::createDefaultSchema();
    SchemaBuilder b;
    auto& f = b.field("name");
    f.type = luxir::api::FieldDef::FieldClass::STRING;
    f.index = IndexMode::MATCH;
    f.column = true;
    f.stored = false;
    schema = b.build(schema.get());
    ch.collection().setSchema(schema);

    ch.index(flatdoc("id", std::string("a"),
                     "name", std::string("alpha")),
             UpdateMessage::COMMIT);
  }

  // Segment 2: enable source storage. Keep the same column settings and
  // store id too, so requesting it forces the shared resource's chunk path.
  {
    auto schema = Schema::createDefaultSchema();
    SchemaBuilder b;
    auto& f = b.field("name");
    f.type = luxir::api::FieldDef::FieldClass::STRING;
    f.index = IndexMode::MATCH;
    f.column = true;
    f.stored = true;
    auto& id = b.field("id");
    id.type = luxir::api::FieldDef::FieldClass::ID;
    id.stored = true;
    schema = b.build(schema.get());
    ch.collection().setSchema(schema);

    ch.index(flatdoc("id", std::string("b"),
                     "name", std::string("beta")),
             UpdateMessage::COMMIT);
  }

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "name"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  // Old segment's "alpha" falls back to the ord column; new segment's "beta"
  // comes from stored fields.
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("a"),
                                        "name", std::string("alpha"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("b"),
                                        "name", std::string("beta"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// Opportunistic stored retrieval: when a request mixes a stored-only field
// (forces chunk decompression) with a field that is also column-stored, the
// column-stored field is pulled from the same chunk.  We can't observe
// the code path directly from the test, but correctness must hold: both
// values come back, including for a value that only exists in the stored
// resource (no column) AND for one that exists in both.
TEST_F(StoredFieldsSearchTest, opportunisticStoredPullsColumnPeerFromChunk) {
  using namespace luxir::test;

  CollectionHelper ch;

  auto schema = Schema::createDefaultSchema();
  // body_t is already stored-only via the default _t suffix (TEXT, no column).
  // Add a STRING field that is BOTH column-stored and STORED.
  {
    SchemaBuilder b;
    auto& f = b.field("author");
    f.type = luxir::api::FieldDef::FieldClass::STRING;
    f.index = IndexMode::NONE;
    f.column = true;
    f.stored = true;
    schema = b.build(schema.get());
  }
  ch.collection().setSchema(schema);

  ch.index(flatdoc("id", std::string("d1"),
                   "body_t", std::string("first body text"),
                   "author", std::string("alice")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d2"),
                   "body_t", std::string("second body text"),
                   "author", std::string("bob")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "body_t", "author"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"),
                                        "body_t", std::string("first body text"),
                                        "author", std::string("alice"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d2"),
                                        "body_t", std::string("second body text"),
                                        "author", std::string("bob"))));

  ch.collection().setSchema(Schema::createDefaultSchema());
}

// Multiple stored TEXT fields sharing the default resource are processed in
// one pass per segment (no N-chunk decompression blow-up).  This test
// covers the grouping path - correctness only; we can't easily observe the
// decompression count from test code.
TEST_F(StoredFieldsSearchTest, multipleFieldsShareResource) {
  using namespace luxir::test;

  CollectionHelper ch;
  ch.collection().setSchema(Schema::createDefaultSchema());

  ch.index(flatdoc("id", std::string("d1"),
                   "a_t", std::string("alpha one"),
                   "b_t", std::string("beta one"),
                   "c_t", std::string("gamma one")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("d2"),
                   "a_t", std::string("alpha two"),
                   "b_t", std::string("beta two"),
                   "c_t", std::string("gamma two")),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id", "a_t", "b_t", "c_t"})
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();

  ASSERT_EQ(2, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d1"),
                                        "a_t", std::string("alpha one"),
                                        "b_t", std::string("beta one"),
                                        "c_t", std::string("gamma one"))));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("d2"),
                                        "a_t", std::string("alpha two"),
                                        "b_t", std::string("beta two"),
                                        "c_t", std::string("gamma two"))));

}
