#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "solux/index/IndexWriter.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/schema/Schema.h"
#include "solux/search/IndexReader.h"
#include "solux/store/Directory.h"
#include "test/SoluxTest.h"

using namespace solux;

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

    // Flatten to (fieldName, firstValue) — convenience for single-valued checks.
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
};

TEST_F(StoredFieldsTest, basicSingleValued) {
  RAMDir dir;
  auto schema = makeSchema();
  {
    IndexWriter iw(dir, [&]() { return schema; });
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
    IndexWriter iw(dir, [&]() { return schema; });
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
    IndexWriter iw(dir, [&]() { return schema; });
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
  std::string big(40 * 1024, 'x');  // 40KB
  for (size_t i = 0; i < big.size(); i++) big[i] = (char)('a' + (i % 26));

  {
    IndexWriter iw(dir, [&]() { return schema; });
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
    IndexWriter iw(dir, [&]() { return schema; });
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

    ASSERT_EQ(iw.getIndexReader()->segments().size(), 2u);

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
  auto activeSchema = schemaWithStored;
  auto schemaProvider = [&activeSchema]() { return activeSchema; };

  {
    IndexWriter iw(dir, schemaProvider);

    // Segment 1: stored on
    activeSchema = schemaWithStored;
    {
      auto& inv = iw.obtainInverter();
      inv.startDoc();
      inv.getIndexHandler("body").index(inv, std::string_view("stored_a"));
      inv.finishDoc();
      iw.releaseInverter(inv, true);
      iw.commit();
    }

    // Segment 2: stored off
    activeSchema = schemaPlain;
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

    activeSchema = schemaWithStored;
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
    IndexWriter iw(dir, [&]() { return schema; });
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
    IndexWriter iw(dir, [&]() { return schema; });
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
    IndexWriter iw(dir, [&]() { return schema; });
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
  // assert that exact ordering — just that every (fN, vN) pair is present.
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
    IndexWriter iw(dir, [&]() { return schema; });
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
