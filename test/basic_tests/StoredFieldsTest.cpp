// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <zstd.h>

#include "luxir/index/IndexRamBudget.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/MergeCostModel.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/FieldMetadataMemory.h"
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
  auto* sfrField = seg.postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(seg.postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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

// Re-adding small dirty sources preserves field names when they encounter
// fields in different orders: A sees body first, B sees title first, and C
// matches A again.
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* defaultReaderField = seg.postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto defaultReader = defaultReaderField ? std::make_unique<StoredFieldsReader>(seg.postingsReader(), *defaultReaderField) : nullptr;
  ASSERT_NE(defaultReader, nullptr);
  auto* paraReaderField = seg.postingsReader().fieldInfo("_stored_embeddings_");
  auto paraReader = paraReaderField ? std::make_unique<StoredFieldsReader>(seg.postingsReader(), *paraReaderField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
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
  auto* sfrField = reader->segments()[0].postingsReader().fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  auto sfr = sfrField ? std::make_unique<StoredFieldsReader>(reader->segments()[0].postingsReader(), *sfrField) : nullptr;
  EXPECT_EQ(sfr, nullptr);
}


// makeSchema() with the default resource configured.
static std::shared_ptr<Schema> withStoredFormat(StoredChunkFormat format) {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED, "whitespace");
  schema->fieldTypeMap["_stored_"] =
      std::make_shared<StoredFieldType>("_stored_", format);
  return schema;
}

// The default resource's reader for a segment, or null if it has none.
static std::unique_ptr<StoredFieldsReader> defaultStored(PostingsReader& pr) {
  auto* info = pr.fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
  return info ? std::make_unique<StoredFieldsReader>(pr, *info) : nullptr;
}

static void addBodyDoc(Inverter& inv, int doc) {
  std::string body = "log line " + std::to_string(doc) + " status=200 path=/api/v1/items";
  inv.startDoc();
  inv.getIndexHandler("body").index(inv, std::string_view(body));
  inv.finishDoc();
}

// Flush one segment of "body" docs numbered [first, first + n).
static void addBodySegment(IndexWriter& iw, int first, int n) {
  auto& inv = iw.obtainInverter();
  for (int i = first; i < first + n; i++) addBodyDoc(inv, i);
  iw.releaseInverter(inv, true);
  iw.commit();
}

static void expectBodies(StoredFieldsReader& r, int n) {
  ASSERT_EQ(r.maxDoc(), n);
  for (int i = 0; i < n; i++) {
    std::string body;
    r.readField(i, "body", [&](std::span<const std::string_view> v) { body = v[0]; });
    EXPECT_EQ("log line " + std::to_string(i) + " status=200 path=/api/v1/items", body);
  }
}

TEST_F(StoredFieldsTest, zstdRoundTripHonorsChunkLimits) {
  RAMDir dir;
  StoredChunkFormat format{StoredChunkFormat::ZSTD, 5, 1024, 4};
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, withStoredFormat(format));
    addBodySegment(iw, 0, 10);
  }
  auto reader = std::make_shared<IndexReader>(dir);
  auto sfr = defaultStored(reader->segments()[0].postingsReader());
  ASSERT_NE(sfr, nullptr);
  EXPECT_TRUE(sfr->format().sameLayout(format));
  EXPECT_EQ(3, sfr->numChunks());  // 4 + 4 + 2 docs
  expectBodies(*sfr, 10);
}

static std::vector<int64_t> chunkFirstDocs(StoredFieldsReader& r) {
  std::vector<int64_t> firstDocs, offsets;
  r.readChunkDirectory(firstDocs, offsets);
  return firstDocs;
}

// Observe the actual path, since recompression can reproduce identical bytes.
static std::vector<bool> mergeStoredCopies(IndexWriter& iw) {
  std::vector<bool> copied;
  Signal::listen("storedFieldsMergeSource", [&](void* resource, void*, void* copy) -> void* {
    if (*(std::string_view*)resource == Postings::STORED_DEFAULT_RESOURCE) {
      copied.push_back(*(bool*)copy);
    }
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("storedFieldsMergeSource"); });
  iw.mergeSegments();
  return copied;
}

TEST_F(StoredFieldsTest, mergeRecompressesOtherFormatsAndCopiesMatching) {
  RAMDir dir;
  StoredChunkFormat zstd{StoredChunkFormat::ZSTD, 3, 64 * 1024, 4};
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, withStoredFormat({}));
    addBodySegment(iw, 0, 3);
    addBodySegment(iw, 3, 3);
    iw.setSchema(withStoredFormat(zstd));
    addBodySegment(iw, 6, 4);  // one full chunk: clean
    iw.mergeSegments();
  }
  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto sfr = defaultStored(reader->segments()[0].postingsReader());
  EXPECT_TRUE(sfr->format().sameLayout(zstd));
  // The two LZ4 sources are re-added (4 docs, then 2 sealed before the copy);
  // the clean ZSTD source is copied.
  EXPECT_EQ((std::vector<int64_t>{0, 4, 6}), chunkFirstDocs(*sfr));
  EXPECT_EQ(1, sfr->numDirtyChunks());
  expectBodies(*sfr, 10);
}

// Small flushes each end in a partial chunk; a merge re-packs them into full
// chunks instead of copying the partial ones.
TEST_F(StoredFieldsTest, mergeRepacksDirtyChunks) {
  RAMDir dir;
  StoredChunkFormat zstd{StoredChunkFormat::ZSTD, 3, 64 * 1024, 16};
  {
    CommitSnapshotRegistry iwSnapshots(dir);
    IndexWriter iw(iwSnapshots, withStoredFormat(zstd));
    for (int i = 0; i < 8; i++) {
      addBodySegment(iw, i * 4, 4);
    }
    {
      auto flushed = iw.snapshots.readers.getReader();
      EXPECT_EQ(1, defaultStored(flushed->segments()[0].postingsReader())->numDirtyChunks());
    }
    iw.mergeSegments();
  }
  auto reader = std::make_shared<IndexReader>(dir);
  ASSERT_EQ(reader->segments().size(), 1u);
  auto sfr = defaultStored(reader->segments()[0].postingsReader());
  EXPECT_EQ((std::vector<int64_t>{0, 16}), chunkFirstDocs(*sfr));
  EXPECT_EQ(0, sfr->numDirtyChunks());
  expectBodies(*sfr, 32);
}

TEST_F(StoredFieldsTest, mergeDirtyChunkThreshold) {
  for (int chunks : {99, 100}) {
    SCOPED_TRACE(chunks);
    RAMDir dir;
    CommitSnapshotRegistry snapshots(dir);
    StoredChunkFormat format;
    format.maxDocsPerChunk = 4;
    IndexWriter iw(snapshots, withStoredFormat(format));
    int docs = (chunks - 1) * 4 + 1;
    addBodySegment(iw, 0, docs);
    addBodySegment(iw, docs, 3);
    {
      auto reader = snapshots.readers.getReader();
      auto source = defaultStored(reader->segments()[0].postingsReader());
      ASSERT_EQ(chunks, source->numChunks());
      ASSERT_EQ(1, source->numDirtyChunks());
    }
    EXPECT_EQ((std::vector<bool>{chunks == 100, false}), mergeStoredCopies(iw));
    auto reader = snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
    auto stored = defaultStored(reader->segments()[0].postingsReader());
    EXPECT_EQ(chunks == 99 ? 99 : 101, stored->numChunks());
    EXPECT_EQ(chunks == 99 ? 0 : 2, stored->numDirtyChunks());
    expectBodies(*stored, docs + 3);
  }
}

TEST_F(StoredFieldsTest, mergeCopiesSmallCleanSources) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  StoredChunkFormat format;
  format.maxDocsPerChunk = 4;
  IndexWriter iw(snapshots, withStoredFormat(format));
  addBodySegment(iw, 0, 4);
  addBodySegment(iw, 4, 4);
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  auto reader = snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto stored = defaultStored(reader->segments()[0].postingsReader());
  EXPECT_EQ(2, stored->numChunks());
  EXPECT_EQ(0, stored->numDirtyChunks());
  expectBodies(*stored, 8);
}

TEST_F(StoredFieldsTest, mergeInheritedDirtCrossesThreshold) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  StoredChunkFormat format;
  format.maxDocsPerChunk = 4;
  IndexWriter iw(snapshots, withStoredFormat(format));
  addBodySegment(iw, 0, 1);
  addBodySegment(iw, 1, 397);  // 100 chunks, one dirty
  EXPECT_EQ((std::vector<bool>{false, true}), mergeStoredCopies(iw));
  {
    auto reader = snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
    auto stored = defaultStored(reader->segments()[0].postingsReader());
    EXPECT_EQ(101, stored->numChunks());
    EXPECT_EQ(2, stored->numDirtyChunks());  // sealed prefix plus inherited dirt
    expectBodies(*stored, 398);
  }
  addBodySegment(iw, 398, 2);
  EXPECT_EQ((std::vector<bool>{false, false}), mergeStoredCopies(iw));
  auto reader = snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto stored = defaultStored(reader->segments()[0].postingsReader());
  EXPECT_EQ(100, stored->numChunks());
  EXPECT_EQ(0, stored->numDirtyChunks());
  expectBodies(*stored, 400);
}

TEST_F(StoredFieldsTest, mergePaddingPreservesDirtyCounts) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  StoredChunkFormat format;
  format.maxDocsPerChunk = 4;
  auto schema = withStoredFormat(format);
  auto plain = withStoredFormat(format);
  plain->fieldTypeMap["body"]->flags_ &= ~FieldType::STORED;
  IndexWriter iw(snapshots, schema);
  int first = 0;
  for (auto [docs, hasStored] : {std::pair{3, false}, {4, true}, {5, false}, {397, true}, {5, false}}) {
    iw.setSchema(hasStored ? schema : plain);
    addBodySegment(iw, first, docs);
    first += docs;
  }
  iw.setSchema(schema);
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  auto reader = snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto stored = defaultStored(reader->segments()[0].postingsReader());
  ASSERT_EQ(106, stored->numChunks());
  // Partial prefix, middle and suffix padding, plus the copied source's dirt.
  EXPECT_EQ(4, stored->numDirtyChunks());
  EXPECT_EQ(414, stored->maxDoc());
  auto firstDocs = chunkFirstDocs(*stored);
  EXPECT_EQ((std::vector<int64_t>{0, 3, 7, 11, 12}),
            (std::vector<int64_t>(firstDocs.begin(), firstDocs.begin() + 5)));
  EXPECT_EQ(409, firstDocs[firstDocs.size() - 2]);
  EXPECT_EQ(413, firstDocs.back());
  for (int doc = 0; doc < stored->maxDoc(); doc++) {
    auto values = readStored(*stored, doc).flat();
    if ((doc >= 3 && doc < 7) || (doc >= 12 && doc < 409)) {
      EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
          {"body", "log line " + std::to_string(doc) + " status=200 path=/api/v1/items"}}), values);
    } else {
      EXPECT_TRUE(values.empty());
    }
  }
}

TEST_F(StoredFieldsTest, mergePreAdoptsLargestCopyFieldTable) {
  for (bool tie : {false, true}) {
    SCOPED_TRACE(tie);
    RAMDir dir;
    CommitSnapshotRegistry snapshots(dir);
    auto schema = makeSchema();
    StoredChunkFormat format;
    format.maxDocsPerChunk = 4;
    schema->fieldTypeMap["_stored_"] = std::make_shared<StoredFieldType>("_stored_", format);
    IndexWriter iw(snapshots, schema);
    auto addFields = [&](int docs, bool titleFirst, std::string_view body) {
      auto& inv = iw.obtainInverter();
      for (int doc = 0; doc < docs; doc++) {
        inv.startDoc();
        if (titleFirst) inv.getIndexHandler("title").index(inv, std::string_view("shared"));
        inv.getIndexHandler("body").index(inv, body);
        if (!titleFirst) inv.getIndexHandler("title").index(inv, std::string_view("shared"));
        inv.finishDoc();
      }
      iw.releaseInverter(inv, true);
      iw.commit();
    };
    std::string body = "shared";
    if (!tie) {
      std::mt19937 random(0);
      body.resize(512);
      for (char& c : body) c = (char)('a' + random() % 26);
      for (size_t i = 63; i < body.size(); i += 64) body[i] = ' ';
    }
    addFields(1, tie, "shared");  // re-added, opposite field order to the winner
    addFields(tie ? 4 : 8, false, "shared");
    addFields(4, true, body);
    {
      auto reader = snapshots.readers.getReader();
      auto second = defaultStored(reader->segments()[1].postingsReader());
      auto third = defaultStored(reader->segments()[2].postingsReader());
      if (tie) {
        ASSERT_EQ(second->chunkRegionBytes(), third->chunkRegionBytes());
      } else {
        ASSERT_GT(second->numChunks(), third->numChunks());
        ASSERT_LT(second->chunkRegionBytes(), third->chunkRegionBytes());
      }
    }
    EXPECT_EQ((std::vector<bool>{false, tie, !tie}), mergeStoredCopies(iw));
    auto reader = snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
    auto stored = defaultStored(reader->segments()[0].postingsReader());
    EXPECT_EQ(tie ? 0 : 1, stored->fieldId("body"));
    EXPECT_EQ(tie ? 1 : 0, stored->fieldId("title"));
    for (int doc = 0; doc < stored->maxDoc(); doc++) {
      EXPECT_TRUE(stored->readField(doc, "body", [&](std::span<const std::string_view> values) {
        ASSERT_EQ(1u, values.size());
        EXPECT_EQ(doc >= stored->maxDoc() - 4 ? body : "shared", values[0]);
      }));
      EXPECT_TRUE(stored->readField(doc, "title", [&](std::span<const std::string_view> values) {
        ASSERT_EQ(1u, values.size());
        EXPECT_EQ("shared", values[0]);
      }));
    }
  }
}

static constexpr StoredChunkFormat DICT_FORMAT{StoredChunkFormat::ZSTD, 3, 1024, 8, 4096};

static std::string trainStoredDictionary() {
  std::string samples;
  std::vector<size_t> sizes;
  for (int doc = 0; doc < 3000; doc++) {
    auto sample = "log line " + std::to_string(doc) + " status=200 path=/api/v1/items";
    sizes.push_back(sample.size());
    samples.append(sample);
  }
  return StoredChunkCompressor::trainDictionary(samples, sizes, DICT_FORMAT.dictionaryBytes);
}

// Exercise the fixed-handle writer directly; inverter dictionary providers
// are separate from the chunk storage contract.
static void writeStoredSegment(RAMDir& dir, const StoredChunkFormat& format,
                               std::shared_ptr<const StoredCompressionDictionary> dictionary,
                               std::span<const std::string> docs, uint64_t segId = 1) {
  PostingsWriter postings(dir, segId, (int32_t)docs.size());
  StoredFieldsWriter stored(postings, Postings::STORED_DEFAULT_RESOURCE, format, std::move(dictionary));
  for (size_t doc = 0; doc < docs.size(); doc++) stored.addValue((int32_t)doc, "body", docs[doc]);
  stored.finish((int32_t)docs.size());
  postings.writeVersionRuns(VersionRunBuilder{});
  postings.finish();
}

// Authored definitions survive reopen; hand-filled fieldTypeMap fixtures do not.
static std::shared_ptr<Schema> dictionarySchema(
    StoredChunkFormat format = DICT_FORMAT, std::optional<StoredChunkFormat> extra = {},
    bool unrelated = false) {
  SchemaBuilder b;
  auto& body = b.field("body");
  body.type = api::FieldDef::FieldClass::TEXT;
  body.stored = true;
  if (unrelated) b.field("price").type = api::FieldDef::FieldClass::INT;
  auto resource = [&](std::string_view name, const StoredChunkFormat& f) {
    auto& def = b.storedResource(name);
    def.codec = (api::StoredResourceDef::Codec)f.codec;
    def.chunk_size_bytes = (int32_t)f.chunkTarget;
    def.max_docs_per_chunk = (int32_t)f.maxDocsPerChunk;
    if (f.codec == StoredChunkFormat::ZSTD) def.zstd_level = f.zstdLevel;
    if (f.dictionaryBytes) {
      def.zstd_dictionary = true;
      def.zstd_dictionary_bytes = (int32_t)f.dictionaryBytes;
    }
  };
  resource("_stored_", format);
  if (extra) resource("_stored_extra_", *extra);
  return b.build();
}

static bool publishStoredDictionary(IndexWriter& iw, std::string_view resource,
                                    const StoredChunkFormat& format,
                                    const std::shared_ptr<const StoredDictionary>& bytes) {
  auto token = iw.storedDictionaries.token(resource, format);
  return token && iw.storedDictionaries.publish(resource, *token, bytes);
}

class StoredBootstrapProbe {
public:
  std::vector<bool> plans;
  std::vector<int64_t> costs;
  std::string sample;
  size_t sampleCount = 0;
  size_t sampleSizeTotal = 0;
  size_t trainedBytes = 0;
  int attempts = 0;
  double trainingMs = 0;
  std::chrono::steady_clock::time_point start;
  std::function<void()> onPlan;
  std::function<void()> onTrained;

  StoredBootstrapProbe() {
    Signal::listen("storedFieldsMergePlan", [this](void* resource, void* cost, void* bootstrap) -> void* {
      if (*(std::string_view*)resource != Postings::STORED_DEFAULT_RESOURCE) return nullptr;
      plans.push_back(*(bool*)bootstrap);
      costs.push_back(*(int64_t*)cost);
      if (onPlan) onPlan();
      return nullptr;
    });
    Signal::listen("storedFieldsDictionarySample", [this](void* resource, void* bytes, void* count) -> void* {
      if (*(std::string_view*)resource != Postings::STORED_DEFAULT_RESOURCE) return nullptr;
      sample = *(std::string_view*)bytes;
      auto sizes = *(std::span<const size_t>*)count;
      sampleCount = sizes.size();
      sampleSizeTotal = std::accumulate(sizes.begin(), sizes.end(), (size_t)0);
      start = std::chrono::steady_clock::now();
      return nullptr;
    });
    Signal::listen("storedFieldsDictionaryTrained", [this](void* resource, void* bytes, void*) -> void* {
      if (*(std::string_view*)resource != Postings::STORED_DEFAULT_RESOURCE) return nullptr;
      trainingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      trainedBytes = ((std::string_view*)bytes)->size();
      attempts++;
      if (onTrained) onTrained();
      return nullptr;
    });
  }

  ~StoredBootstrapProbe() {
    Signal::unlisten("storedFieldsMergePlan");
    Signal::unlisten("storedFieldsDictionarySample");
    Signal::unlisten("storedFieldsDictionaryTrained");
  }
};

TEST_F(StoredFieldsTest, bootstrapTrainsPublishesSharesAndRetrainsAfterClear) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  addBodySegment(iw, 0, 8192);
  addBodySegment(iw, 8192, 8192);
  int64_t plainBytes = 0;
  for (auto& seg : snapshots.readers.getReader()->segments()) {
    plainBytes += defaultStored(seg.postingsReader())->chunkRegionBytes();
  }
  StoredBootstrapProbe probe;
  EXPECT_EQ((std::vector<bool>{false, false}), mergeStoredCopies(iw));
  auto first = iw.storedDictionaries.lookup("_stored_", DICT_FORMAT);
  ASSERT_NE(nullptr, first);
  EXPECT_LT(defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader())->chunkRegionBytes(), plainBytes);
  EXPECT_EQ(StoredChunkCompressor::dictionarySampleBudget(DICT_FORMAT), probe.sample.size());
  EXPECT_EQ(1, probe.attempts);
  EXPECT_EQ(probe.sample.size(), probe.sampleSizeTotal);
  EXPECT_NE(std::string::npos, probe.sample.find("log line 0 "));
  EXPECT_NE(std::string::npos, probe.sample.find("log line 8192 ")); // both source halves sampled
  addBodySegment(iw, 16384, 8);
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(first->bytes(), defaultStored(reader->segments().back().postingsReader())->dictionary());
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  EXPECT_EQ(1, probe.attempts);
  addBodySegment(iw, 16392, 4096);
  auto changed = DICT_FORMAT;
  changed.dictionaryBytes *= 2;
  iw.setSchema(dictionarySchema(changed));
  EXPECT_EQ((std::vector<bool>{false, false}), mergeStoredCopies(iw));
  auto next = iw.storedDictionaries.lookup("_stored_", changed);
  ASSERT_NE(nullptr, next);
  EXPECT_NE(first->bytes(), next->bytes());
  EXPECT_EQ(2, probe.attempts);
  EXPECT_EQ((std::vector<bool>{true, false, true}), probe.plans);
  auto stored = defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader());
  expectBodies(*stored, 20488);
}

TEST_F(StoredFieldsTest, bootstrapAdmissionPricesOnlyAnEmptySlotAndPinsTheDecision) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  addBodySegment(iw, 0, 8);
  addBodySegment(iw, 8, 8);
  StoredBootstrapProbe probe;
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  EXPECT_EQ(0, probe.attempts);
  EXPECT_LT(probe.sample.size(), StoredChunkCompressor::dictionarySampleBudget(DICT_FORMAT));
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
  addBodySegment(iw, 16, 8);
  probe.onPlan = [&] {
    auto changed = DICT_FORMAT;
    changed.dictionaryBytes *= 2;
    iw.setSchema(dictionarySchema(changed));
    iw.setSchema(dictionarySchema());
  };
  size_t sampleBytes = probe.sample.size();
  EXPECT_EQ((std::vector<bool>{true, false}), mergeStoredCopies(iw));
  EXPECT_EQ((std::vector<bool>{true, false}), probe.plans);
  ASSERT_EQ(2u, probe.costs.size());
  EXPECT_EQ(StoredChunkCompressor::dictionaryTrainingBytes(DICT_FORMAT), probe.costs[0] - probe.costs[1]);
  EXPECT_EQ(sampleBytes, probe.sample.size()); // no unpriced sampling after the clear
  EXPECT_EQ(0, probe.attempts);
  EXPECT_EQ(nullptr, iw.storedDictionaries.lookup("_stored_", DICT_FORMAT));
}

TEST_F(StoredFieldsTest, bootstrapManyTinyDocsHaveBoundedSamples) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  for (int source = 0; source < 2; source++) {
    auto& inv = iw.obtainInverter();
    for (int doc = 0; doc < 45000; doc++) {
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("x"));
      inv.finishDoc();
    }
    iw.releaseInverter(inv, true);
    iw.commit();
  }
  StoredBootstrapProbe probe;
  iw.mergeSegments();
  EXPECT_EQ(1, probe.attempts);
  EXPECT_EQ(StoredChunkCompressor::dictionarySampleBudget(DICT_FORMAT), probe.sample.size());
  EXPECT_EQ(probe.sample.size(), probe.sampleSizeTotal);
  EXPECT_LE(probe.sampleCount, StoredChunkCompressor::MAX_DICTIONARY_SAMPLES);
  EXPECT_GT(probe.sample.size() / 5, StoredChunkCompressor::MAX_DICTIONARY_SAMPLES);
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(90000, reader->segments()[0].maxDoc());
}

TEST_F(StoredFieldsTest, bootstrapLosingPublicationUsesWinnerOrMovedEpoch) {
  auto format = DICT_FORMAT;
  format.dictionaryBytes = 1024;
  for (bool moveEpoch : {false, true}) {
    SCOPED_TRACE(moveEpoch);
    RAMDir dir;
    CommitSnapshotRegistry snapshots(dir);
    IndexWriter iw(snapshots, dictionarySchema(format));
    addBodySegment(iw, 0, 2048);
    addBodySegment(iw, 2048, 2048);
    auto winner = std::make_shared<const StoredDictionary>(std::string(1024, 'w'));
    StoredBootstrapProbe probe;
    probe.onTrained = [&] {
      if (moveEpoch) {
        auto changed = format;
        changed.dictionaryBytes *= 2;
        iw.setSchema(dictionarySchema(changed));
        iw.setSchema(dictionarySchema(format));
      } else {
        EXPECT_TRUE(publishStoredDictionary(iw, "_stored_", format, winner));
      }
    };
    EXPECT_EQ((std::vector<bool>{moveEpoch, moveEpoch}), mergeStoredCopies(iw));
    EXPECT_EQ(1, probe.attempts);
    EXPECT_GT(probe.trainedBytes, 0u);
    auto stored = defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader());
    EXPECT_EQ(moveEpoch ? std::string_view{} : winner->bytes(), stored->dictionary());
    expectBodies(*stored, 4096);
  }
}

TEST_F(StoredFieldsTest, bootstrapPublicationSurvivesAFailedMerge) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  auto format = DICT_FORMAT;
  format.dictionaryBytes = 1024;
  IndexWriter iw(snapshots, dictionarySchema(format));
  addBodySegment(iw, 0, 2048);
  addBodySegment(iw, 2048, 2048);
  {
    ExpectLog quiet("injected dictionary merge failure");
    Signal::listen("storedFieldsMergeSource", [](void*, void*, void*) -> void* {
      throw std::runtime_error("injected dictionary merge failure");
    });
    auto cleanup = scope_guard([] { Signal::unlisten("storedFieldsMergeSource"); });
    iw.mergeSegments();
  }
  ASSERT_EQ(2u, snapshots.readers.getReader()->segments().size());
  auto dictionary = iw.storedDictionaries.lookup("_stored_", format);
  ASSERT_NE(nullptr, dictionary);
  addBodySegment(iw, 4096, 8);
  EXPECT_EQ((std::vector<bool>{false, false, true}), mergeStoredCopies(iw));
  EXPECT_EQ(dictionary->bytes(), defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader())->dictionary());
}

TEST_F(StoredFieldsTest, bootstrapFullSampleReservesTrainingPeakAndCapsDictionary) {
  RAMDir dir;
  IndexRamBudget budget(64 * 1024 * 1024);
  CommitSnapshotRegistry snapshots(dir);
  auto format = DICT_FORMAT;
  format.dictionaryBytes = 262144;
  IndexWriter iw(snapshots, dictionarySchema(format), &budget);
  auto body = bigStoredValue();
  for (int source = 0; source < 2; source++) {
    auto& inv = iw.obtainInverter();
    for (int doc = 0; doc < 128; doc++) {
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view(body));
      inv.finishDoc();
    }
    iw.releaseInverter(inv, true);
    iw.commit();
  }
  StoredBootstrapProbe probe;
  probe.onTrained = [&] { EXPECT_GE(budget.reservedBytes(), probe.costs[0]); };
  iw.mergeSegments();
  auto dictionary = iw.storedDictionaries.lookup("_stored_", format);
  ASSERT_NE(nullptr, dictionary);
  EXPECT_EQ(1, probe.attempts);
  EXPECT_EQ(8u * 1024 * 1024, probe.sample.size());
  EXPECT_LE(dictionary->bytes().size(), probe.sample.size() / 100);
  RecordProperty("dictionary_training_ms", std::to_string(probe.trainingMs));
  RecordProperty("sample_bytes", std::to_string(probe.sample.size()));
  RecordProperty("bootstrap_batch_bytes", std::to_string(probe.costs[0]));
  auto stored = defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader());
  EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{{"body", body}}), readStored(*stored, 255).flat());
}

TEST_F(StoredFieldsTest, registryFreshFlushesHaveNoDictionary) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  addBodySegment(iw, 0, 8);
  addBodySegment(iw, 8, 8);
  auto reader = snapshots.readers.getReader();
  for (auto& seg : reader->segments()) EXPECT_TRUE(defaultStored(seg.postingsReader())->dictionary().empty());
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  EXPECT_TRUE(defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader())->dictionary().empty());
}

TEST_F(StoredFieldsTest, registryFlushesShareDictionaryAndMergeCopies) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
  addBodySegment(iw, 0, 8);
  addBodySegment(iw, 8, 8);
  auto reader = snapshots.readers.getReader();
  ASSERT_EQ(2u, reader->segments().size());
  for (auto& seg : reader->segments()) EXPECT_EQ(bytes->bytes(), defaultStored(seg.postingsReader())->dictionary());
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  auto merged = defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader());
  EXPECT_EQ(bytes->bytes(), merged->dictionary());
  expectBodies(*merged, 16);
  merged.reset();
  iw.testDeleteAllData();
  EXPECT_EQ(nullptr, iw.storedDictionaries.lookup("_stored_", DICT_FORMAT));
  EXPECT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
}

TEST_F(StoredFieldsTest, registryWriterKeepsPinAfterClear) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
  auto& inv = iw.obtainInverter();
  addBodyDoc(inv, 0); // creates and pins the resource writer, before the clear
  auto changed = DICT_FORMAT;
  changed.dictionaryBytes *= 2;
  iw.setSchema(dictionarySchema(changed));
  EXPECT_EQ(nullptr, iw.storedDictionaries.lookup("_stored_", changed));
  for (int doc = 1; doc < 8; doc++) addBodyDoc(inv, doc);
  iw.releaseInverter(inv, true);
  iw.commit();
  EXPECT_EQ(bytes->bytes(), defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader())->dictionary());
  addBodySegment(iw, 8, 8);
  EXPECT_EQ((std::vector<bool>{false, true}), mergeStoredCopies(iw));
  auto merged = defaultStored(snapshots.readers.getReader()->segments()[0].postingsReader());
  EXPECT_TRUE(merged->dictionary().empty());
  expectBodies(*merged, 16);
}

TEST_F(StoredFieldsTest, registrySchemaInvalidatesOnlyAffectedPolicy) {
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  for (int edit = 0; edit < 6; edit++) {
    SCOPED_TRACE(edit);
    RAMDir dir;
    CommitSnapshotRegistry snapshots(dir);
    IndexWriter iw(snapshots, dictionarySchema(DICT_FORMAT, DICT_FORMAT));
    ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
    ASSERT_TRUE(publishStoredDictionary(iw, "_stored_extra_", DICT_FORMAT, bytes));
    auto original = iw.storedDictionaries.lookup("_stored_", DICT_FORMAT);
    auto before = *iw.storedDictionaries.token("_stored_extra_", DICT_FORMAT);
    std::optional<StoredChunkFormat> changed = DICT_FORMAT;
    if (edit == 0) changed->dictionaryBytes *= 2;
    if (edit == 1) changed->dictionaryBytes = 0;
    if (edit == 2) { changed->codec = StoredChunkFormat::LZ4; changed->dictionaryBytes = 0; }
    if (edit == 3) changed.reset();
    if (edit == 4) { changed->zstdLevel = 7; changed->chunkTarget *= 2; changed->maxDocsPerChunk *= 2; }
    iw.setSchema(dictionarySchema(DICT_FORMAT, changed, edit == 5));
    EXPECT_EQ(original, iw.storedDictionaries.lookup("_stored_", DICT_FORMAT));
    if (edit < 4) {
      EXPECT_EQ(nullptr, iw.storedDictionaries.lookup("_stored_extra_", DICT_FORMAT));
      iw.setSchema(dictionarySchema(DICT_FORMAT, DICT_FORMAT));
      EXPECT_NE(before.epoch, iw.storedDictionaries.token("_stored_extra_", DICT_FORMAT)->epoch);
      EXPECT_FALSE(iw.storedDictionaries.publish("_stored_extra_", before, bytes));
    } else {
      EXPECT_EQ(before.epoch, iw.storedDictionaries.token("_stored_extra_", *changed)->epoch);
      EXPECT_EQ(bytes->bytes(), iw.storedDictionaries.lookup("_stored_extra_", *changed)->bytes());
      EXPECT_EQ(bytes->bytes(), iw.storedDictionaries.lookup("_stored_extra_", DICT_FORMAT)->bytes());
    }
  }
}

TEST_F(StoredFieldsTest, registryPublishChecksEpochFormatAndEmptySlot) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  auto before = *iw.storedDictionaries.token("_stored_", DICT_FORMAT);
  auto changed = DICT_FORMAT;
  changed.zstdLevel++;
  snapshots.onChange = [&] {
    EXPECT_FALSE(iw.storedDictionaries.publish("_stored_", before, bytes));
  };
  iw.setSchema(dictionarySchema(changed));
  snapshots.onChange = {};
  EXPECT_FALSE(iw.storedDictionaries.publish("_stored_", before, bytes)); // same epoch, different format
  changed.dictionaryBytes *= 2;
  iw.setSchema(dictionarySchema(changed));
  iw.setSchema(dictionarySchema()); // policy A -> B -> A, with an empty slot throughout
  EXPECT_FALSE(iw.storedDictionaries.publish("_stored_", before, bytes));
  auto current = *iw.storedDictionaries.token("_stored_", DICT_FORMAT);
  EXPECT_NE(before.epoch, current.epoch);
  EXPECT_TRUE(iw.storedDictionaries.publish("_stored_", current, bytes));
  EXPECT_FALSE(iw.storedDictionaries.publish("_stored_", current, bytes));
}

TEST_F(StoredFieldsTest, registryPreparedDictionariesSharedBySettings) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, dictionarySchema());
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
  std::array<std::shared_ptr<const StoredCompressionDictionary>, 8> pins;
  std::barrier start((int)pins.size());
  std::vector<std::thread> threads;
  for (size_t i = 0; i < pins.size(); i++) threads.emplace_back([&, i] {
    start.arrive_and_wait();
    pins[i] = iw.storedDictionaries.lookup("_stored_", DICT_FORMAT);
  });
  for (auto& thread : threads) thread.join();
  ASSERT_NE(nullptr, pins[0]);
  for (auto& pin : pins) EXPECT_EQ(pins[0], pin);
  for (int part : {0, 1}) {
    auto other = DICT_FORMAT;
    if (part == 0) other.zstdLevel++;
    else other.chunkTarget *= 2;
    auto prepared = iw.storedDictionaries.lookup("_stored_", other);
    EXPECT_NE(pins[0], prepared);
    EXPECT_EQ(prepared, iw.storedDictionaries.lookup("_stored_", other));
    EXPECT_TRUE(prepared->matches(other));
    EXPECT_EQ(bytes->bytes(), prepared->bytes());
  }
}

TEST_F(StoredFieldsTest, registryUnchangedOnFailedSchemaPublication) {
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  for (auto signal : {"manifestSynced", "manifestDurable"}) {
    SCOPED_TRACE(signal);
    RAMDir dir;
    CommitSnapshotRegistry snapshots(dir);
    IndexWriter iw(snapshots, dictionarySchema());
    ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, bytes));
    auto before = *iw.storedDictionaries.token("_stored_", DICT_FORMAT);
    auto pinned = iw.storedDictionaries.lookup("_stored_", DICT_FORMAT);
    Signal::listen(signal, [&](void* source, void*, void*) -> void* {
      if (source == &iw) throw std::runtime_error("injected schema publication failure");
      return nullptr;
    });
    auto cleanup = scope_guard([&] { Signal::unlisten(signal); });
    auto changed = DICT_FORMAT;
    changed.dictionaryBytes *= 2;
    EXPECT_THROW(iw.setSchema(dictionarySchema(changed)), std::runtime_error);
    EXPECT_EQ(before.epoch, iw.storedDictionaries.token("_stored_", DICT_FORMAT)->epoch);
    EXPECT_EQ(pinned, iw.storedDictionaries.lookup("_stored_", DICT_FORMAT));
  }
}

TEST_F(StoredFieldsTest, registryReopenUsesLargestLiveChunkRegionThenLowestId) {
  auto a = std::make_shared<const StoredDictionary>(std::string(4096, 'a'));
  auto b = std::make_shared<const StoredDictionary>(std::string(4096, 'b'));
  for (bool tie : {false, true}) {
    SCOPED_TRACE(tie);
    RAMDir dir;
    int64_t liveBytes = 0;
    std::shared_ptr<const StoredCompressionDictionary> pin;
    {
      CommitSnapshotRegistry snapshots(dir);
      IndexWriter iw(snapshots, dictionarySchema());
      ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", DICT_FORMAT, a));
      addBodySegment(iw, 0, tie ? 8 : 128);
      auto changed = DICT_FORMAT;
      changed.dictionaryBytes *= 2;
      iw.setSchema(dictionarySchema(changed));
      ASSERT_TRUE(publishStoredDictionary(iw, "_stored_", changed, b));
      if (tie) {
        addBodySegment(iw, 0, 8);
      } else {
        std::mt19937 random(0);
        std::string body(1024, ' ');
        for (size_t i = 0; i < body.size(); i++) body[i] = i % 64 == 63 ? ' ' : (char)('a' + random() % 26);
        auto& inv = iw.obtainInverter();
        for (int doc = 0; doc < 8; doc++) {
          inv.startDoc();
          inv.getIndexHandler("body").index(inv, std::string_view(body));
          inv.finishDoc();
        }
        iw.releaseInverter(inv, true);
        iw.commit();
      }
      auto reader = snapshots.readers.getReader();
      auto first = defaultStored(reader->segments()[0].postingsReader());
      auto second = defaultStored(reader->segments()[1].postingsReader());
      if (tie) {
        ASSERT_EQ(first->chunkRegionBytes(), second->chunkRegionBytes());
      } else {
        ASSERT_GT(first->numChunks(), second->numChunks());
        ASSERT_GT(first->maxDoc(), second->maxDoc());
        ASSERT_LT(first->chunkRegionBytes(), second->chunkRegionBytes());
      }
      liveBytes = second->chunkRegionBytes();
      iw.setSchema(dictionarySchema()); // leave no current handle to carry through reopen
      EXPECT_EQ(nullptr, iw.storedDictionaries.lookup("_stored_", DICT_FORMAT));
    }
    // An unreferenced, larger segment must not seed the registry.
    auto orphanBytes = std::make_shared<const StoredDictionary>(std::string(4096, 'c'));
    auto orphan = std::make_shared<const StoredCompressionDictionary>(orphanBytes, DICT_FORMAT);
    std::vector<std::string> docs(4096, "orphan");
    writeStoredSegment(dir, DICT_FORMAT, orphan, docs, 99);
    {
      PostingsReader reader(dir, 99);
      ASSERT_GT(defaultStored(reader)->chunkRegionBytes(), liveBytes);
    }
    {
      CommitSnapshotRegistry snapshots(dir);
      IndexWriter iw(snapshots);
      pin = iw.storedDictionaries.lookup("_stored_", DICT_FORMAT);
      ASSERT_NE(nullptr, pin);
      EXPECT_EQ((tie ? a : b)->bytes(), pin->bytes());
      addBodySegment(iw, 136, 8);
      auto reader = snapshots.readers.getReader();
      EXPECT_EQ(pin->bytes(), defaultStored(reader->segments().back().postingsReader())->dictionary());
    }
    dir.clear(); // prepared dictionary must own bytes after segment mappings die
    RAMDir output;
    writeStoredSegment(output, DICT_FORMAT, pin, docs);
    PostingsReader reader(output, 1);
    auto stored = defaultStored(reader);
    EXPECT_EQ(pin->bytes(), stored->dictionary());
    EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{{"body", "orphan"}}), readStored(*stored, 0).flat());
  }
}

TEST_F(StoredFieldsTest, pinnedDictionaryRoundTrip) {
  auto trained = trainStoredDictionary();
  ASSERT_FALSE(trained.empty());
  ASSERT_NE(0u, ZSTD_getDictID_fromDict(trained.data(), trained.size()));
  std::vector<std::string> docs(16, "log line status=200 path=/api/v1/items" + std::string(300, 'x'));
  docs[5] = bigStoredValue();
  for (int level : {1, 7}) {
    for (uint32_t target : {1024u, 4096u}) {
      SCOPED_TRACE(std::to_string(level) + "/" + std::to_string(target));
      auto format = DICT_FORMAT;
      format.zstdLevel = level;
      format.chunkTarget = target;
      format.maxDocsPerChunk = 128;
      auto bytes = std::make_shared<const StoredDictionary>(trained);
      std::weak_ptr<const StoredDictionary> pin = bytes;
      auto prepared = std::make_shared<const StoredCompressionDictionary>(bytes, format);
      bytes.reset();
      ASSERT_FALSE(pin.expired());
      RAMDir dir;
      writeStoredSegment(dir, format, std::move(prepared), docs);
      EXPECT_TRUE(pin.expired());
      PostingsReader postings(dir, 1);
      auto stored = defaultStored(postings);
      ASSERT_NE(nullptr, stored);
      EXPECT_EQ(trained, stored->dictionary());
      EXPECT_TRUE(stored->format().sameLayout(format));
      EXPECT_EQ(0u, stored->format().dictionaryBytes);  // policy is not segment metadata
      EXPECT_GE(stored->maxChunkBytes(), (int64_t)docs[5].size());
      for (size_t doc = 0; doc < docs.size(); doc++) {
        EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{{"body", docs[doc]}}),
                  readStored(*stored, (int32_t)doc).flat());
      }
      std::vector<int64_t> firstDocs, offsets;
      stored->readChunkDirectory(firstDocs, offsets);
      offsets.push_back(stored->chunkRegionBytes());
      for (size_t chunk = 0; chunk + 1 < offsets.size(); chunk++) {
        const char* frame = stored->chunkRegionPtr() + offsets[chunk] + sizeof(int32_t);
        size_t size = (size_t)(offsets[chunk + 1] - offsets[chunk]) - sizeof(int32_t);
        EXPECT_EQ(0u, ZSTD_getDictID_fromFrame(frame, size));
      }
    }
  }
}

TEST_F(StoredFieldsTest, copyRequiresMatchingLayoutAndDictionaryBytes) {
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  ASSERT_FALSE(bytes->bytes().empty());
  auto prepared = std::make_shared<const StoredCompressionDictionary>(bytes, DICT_FORMAT);
  RAMDir dir;
  PostingsWriter postings(dir, 1);
  StoredFieldsWriter stored(postings, "_stored_", DICT_FORMAT, prepared);
  auto other = DICT_FORMAT;
  other.dictionaryBytes = 0;
  other.zstdLevel = 7;
  EXPECT_TRUE(DICT_FORMAT.sameLayout(other));
  std::string identical(bytes->bytes());
  EXPECT_TRUE(stored.canCopyChunks(other, identical));
  identical.back() ^= 1;
  EXPECT_FALSE(stored.canCopyChunks(other, identical));
  EXPECT_FALSE(stored.canCopyChunks(other, {}));
  for (int part : {0, 1, 2}) {
    other = DICT_FORMAT;
    if (part == 0) other.codec = StoredChunkFormat::LZ4;
    if (part == 1) other.chunkTarget *= 2;
    if (part == 2) other.maxDocsPerChunk *= 2;
    EXPECT_FALSE(stored.canCopyChunks(other, bytes->bytes()));
  }
}

TEST_F(StoredFieldsTest, emptyDictionarySourcesCopyAcrossPolicyChanges) {
  RAMDir dir;
  CommitSnapshotRegistry snapshots(dir);
  IndexWriter iw(snapshots, withStoredFormat(DICT_FORMAT));
  addBodySegment(iw, 0, 8);
  auto plain = DICT_FORMAT;
  plain.dictionaryBytes = 0;
  iw.setSchema(withStoredFormat(plain));
  addBodySegment(iw, 8, 8);
  iw.setSchema(withStoredFormat(DICT_FORMAT));
  EXPECT_EQ((std::vector<bool>{true, true}), mergeStoredCopies(iw));
  auto reader = snapshots.readers.getReader();
  auto stored = defaultStored(reader->segments()[0].postingsReader());
  EXPECT_TRUE(stored->dictionary().empty());
  expectBodies(*stored, 16);
}

TEST_F(StoredFieldsTest, corruptDictionarySizeRejected) {
  RAMDir dir;
  std::vector<std::string> docs{"log line status=200 path=/api/v1/items"};
  writeStoredSegment(dir, DICT_FORMAT, {}, docs);
  PostingsReader postings(dir, 1);
  const auto* field = postings.fieldInfo("_stored_");
  auto input = postings.getInputStream(field->columnLoc.filenum());
  const char* ptr = input.ptr((int64_t)field->columnLoc.offset() + field->columnMetaOff);
  const char* end = input.ptr(input.size());
  InputStream::readVlong(ptr, end);  // max chunk bytes
  InputStream::readVint(ptr, end);   // dirty count
  ptr++;                           // codec
  InputStream::readVint(ptr, end);  // chunk target
  InputStream::readVint(ptr, end);  // max docs
  ASSERT_GE(end - ptr, 5);
  memcpy(const_cast<char*>(ptr), "\xff\xff\xff\xff\x0f", 5);  // UINT32_MAX
  PostingsReader corrupt(dir, 1);
  EXPECT_THROW(corrupt.fieldInfo("_stored_"), std::runtime_error);
  EXPECT_EQ(0u, corrupt.fieldCacheStats().entries);
}

TEST_F(StoredFieldsTest, storedMetadataCacheSharedAndAccounted) {
  RAMDir dir;
  auto bytes = std::make_shared<const StoredDictionary>(trainStoredDictionary());
  auto prepared = std::make_shared<const StoredCompressionDictionary>(bytes, DICT_FORMAT);
  std::vector<std::string> docs{"log line status=200 path=/api/v1/items"};
  writeStoredSegment(dir, DICT_FORMAT, prepared, docs);
  auto before = fieldMetadataMemory.entryBytes.load();
  {
    PostingsReader postings(dir, 1);
    EXPECT_EQ(0u, postings.fieldCacheStats().entries);
    std::array<const PostingsReader::Field*, 8> fields{};
    std::barrier start(8);
    std::vector<std::jthread> threads;
    for (size_t i = 0; i < fields.size(); i++) {
      threads.emplace_back([&, i] {
        start.arrive_and_wait();
        fields[i] = postings.fieldInfo("_stored_");
      });
    }
    threads.clear();
    ASSERT_NE(nullptr, fields[0]);
    ASSERT_TRUE(fields[0]->stored->ddict.has_value());
    for (auto* field : fields) EXPECT_EQ(fields[0], field);
    auto stats = postings.fieldCacheStats();
    EXPECT_EQ(1u, stats.entries);
    EXPECT_GT(stats.entryBytes, sizeof(PostingsReader::Field) + sizeof(StoredFieldsMetadata)
              + ZSTD_sizeof_DDict(fields[0]->stored->ddict->get()));
    EXPECT_GT(stats.bytes, stats.entryBytes);
    EXPECT_EQ(before + stats.entryBytes, fieldMetadataMemory.entryBytes.load());
  }
  EXPECT_EQ(before, fieldMetadataMemory.entryBytes.load());
}

// End-to-end: a STORED TEXT field should come back in search results when
// requested via TopDocs.fields.
class StoredFieldsSearchTest : public luxir::LuxirTest {};

TEST_F(StoredFieldsSearchTest, bootstrapSkipsDeletedEmptyAndPaddingDocs) {
  using namespace luxir::test;
  CollectionHelper helper;
  helper.collection().setSchema(dictionarySchema());
  std::vector<Doc> docs;
  for (int doc = 0; doc < 32; doc++) docs.push_back(flatdoc("id", std::to_string(doc)));
  docs[5] = flatdoc("id", "5", "body", "keep-first");
  docs[9] = flatdoc("id", "9", "body", "DELETED_MARKER " + std::string(40000, ' '));
  docs[23] = flatdoc("id", "23", "body", "keep-last");
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "without-resource"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("9", UpdateMessage::COMMIT).success);
  StoredBootstrapProbe probe;
  helper.getIndexWriter()->mergeSegments();
  EXPECT_EQ(0, probe.attempts);
  EXPECT_EQ(27u, probe.sample.size()); // two field headers and their nonempty values
  EXPECT_NE(std::string::npos, probe.sample.find("keep-first"));
  EXPECT_NE(std::string::npos, probe.sample.find("keep-last"));
  EXPECT_EQ(std::string::npos, probe.sample.find("DELETED_MARKER"));
}

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
    f2.multi = api::FieldDef::MultiMode::DEFAULT;
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
  // fromProto; the named one is declared with ZSTD chunks.
  auto schema = Schema::createDefaultSchema();
  SchemaBuilder b;
  b.storedResource("_stored_embeddings_").codec = luxir::api::StoredResourceDef::Codec::ZSTD;
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
  auto* resource = dynamic_cast<StoredFieldType*>(schema->getFieldTypePtr("_stored_embeddings_"));
  ASSERT_NE(nullptr, resource);
  ASSERT_EQ(StoredChunkFormat::ZSTD, resource->format_.codec);
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

  for (int parallel : {-1, 0, 1}) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q")
        .allQuery()
        .fields({"id", "name"})
        .limit(-1);
    req->execute(parallel);
    ASSERT_OK(req);
    auto docs = req->getDocs();

    ASSERT_EQ(2, docs.size());
    // Old segment's "alpha" falls back to the ord column; new segment's "beta"
    // comes from stored fields.
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("a"),
                                          "name", std::string("alpha"))));
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", std::string("b"),
                                          "name", std::string("beta"))));

  }

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
