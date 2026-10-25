// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The _source_ store through the engine: capture of documents received as Maps,
// merges that reference or relocate source files, file lifetime, damage, and the
// schema switches. HTTP capture and export are in SourceHttpTest.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/SourceReader.h"
#include "luxir/source/SourceFormat.h"
#include "luxir/store/OutputStream.h"
#include "luxir/store/SourceFiles.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

namespace {

using Resource = api::StoredResourceDef;

class SourceStoreTest : public LuxirTest {
protected:
  // Declares _source_ (mode=set) with `configure` applied to the entry.
  static void enableSource(CollectionHelper& helper, const std::function<void(Resource&)>& configure = {}) {
    SchemaBuilder b;
    auto& source = b.storedResource(Postings::SOURCE_RESOURCE);
    if (configure) configure(source);
    b.set(helper.collection());
  }

  static IndexResult forceMerge(CollectionHelper& helper) {
    CollectionHelper::UpdateBuilder request;
    request.commit(true, 1);
    return helper.submit(request);
  }

  // The _source of every live document, in doc order, with "" for a null.
  static std::vector<std::string> sources(CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields({"_source"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    std::vector<std::string> out;
    for (const auto& doc : req->getDocs()) {
      const auto* value = find(doc, "_source");
      const auto* text = value ? std::get_if<std::string>(value) : nullptr;
      out.push_back(text ? *text : std::string());
    }
    return out;
  }

  static std::vector<std::string> sourceFileNames(const api::IndexInfo& info) {
    std::vector<std::string> names;
    for (const auto& file : manifestFiles(info)) {
      if (isSourceFile(file.name) && !isSourceMapFile(file.name)) names.emplace_back(file.name);
    }
    std::ranges::sort(names);
    return names;
  }

  static bool fileExists(Directory& dir, std::string_view name) {
    std::vector<Directory::FileInfo> files;
    dir.listFiles(files);
    return std::ranges::any_of(files, [&](const auto& file) { return file.name == name; });
  }

  static bool waitGone(Directory& dir, std::string_view name) {
    for (int i = 0; i < 200 && fileExists(dir, name); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return !fileExists(dir, name);
  }

  // Overwrites a file with bytes that are not a source file.
  static void damage(Directory& dir, const std::string& name) {
    auto file = dir.createFile(name);
    OutputStream out(file.get());
    std::string junk(64, 'x');
    out.write(junk.data(), junk.size());
    out.close();
    dir.finishFile(*file);
    const std::string names[] = {name};
    dir.sync(names);  // a listed file must be synced before a commit publishes it
  }
};

}  // namespace

TEST_F(SourceStoreTest, keepsMapsAcrossMergesAndRetiresReplacedFiles) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "n_i", 42), flatdoc("id", "b", "s_s", "x y")},
                              UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "c", "k_is", std::vector<int64_t>{1, 2})}, UpdateMessage::COMMIT).success);
  auto before = sourceFileNames(*readDurableIndexInfo(iw->dir));
  ASSERT_EQ(2u, before.size());
  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(forceMerge(helper).success);

  // Default thresholds: flushed files are tiny, so the merge relocates into one new file.
  auto after = sourceFileNames(*readDurableIndexInfo(iw->dir));
  ASSERT_EQ(1u, after.size());
  EXPECT_FALSE(std::ranges::contains(before, after[0]));
  EXPECT_EQ((std::vector<std::string>{R"({"id":"b","s_s":"x y"})", R"({"id":"c","k_is":[1,2]})"}), sources(helper));
  for (const auto& name : before) EXPECT_TRUE(waitGone(iw->dir, name)) << name;
}

TEST_F(SourceStoreTest, mergeReferencesFilesWorthKeeping) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper, [](Resource& r) { r.min_file_bytes = 0; r.max_files = 100; });
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
  auto before = sourceFileNames(*readDurableIndexInfo(iw->dir));
  ASSERT_TRUE(forceMerge(helper).success);
  // Fully live and above the thresholds: the merged segment lists the inputs' files.
  EXPECT_EQ(before, sourceFileNames(*readDurableIndexInfo(iw->dir)));
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a"})", R"({"id":"b"})", R"({"id":"c"})"}), sources(helper));
}

TEST_F(SourceStoreTest, maxFilesBoundsTheFilesASegmentReferences) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(100);
  enableSource(helper, [](Resource& r) { r.min_file_bytes = 0; r.max_files = 2; });
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", std::to_string(i)), UpdateMessage::COMMIT).success);
  }
  ASSERT_EQ(5u, sourceFileNames(*readDurableIndexInfo(iw->dir)).size());
  ASSERT_TRUE(forceMerge(helper).success);
  // At most max_files referenced plus the one the merge wrote.
  EXPECT_LE(sourceFileNames(*readDurableIndexInfo(iw->dir)).size(), 3u);
  EXPECT_EQ(5u, sources(helper).size());
}

TEST_F(SourceStoreTest, relocationRepacksPartialBlocks) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper);
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", std::to_string(i), "t_s", "short"), UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(forceMerge(helper).success);
  auto files = sourceFileNames(*readDurableIndexInfo(iw->dir));
  ASSERT_EQ(1u, files.size());
  auto input = iw->dir.openFile(files[0]);
  ASSERT_NE(nullptr, input);
  auto reader = source::FileReader::open(input->read());
  ASSERT_TRUE(reader.has_value());
  // Three flushes' partial blocks fold into one block instead of three.
  EXPECT_EQ(1u, reader->blocks().size());
  EXPECT_EQ(3u, reader->docCount());
}

TEST_F(SourceStoreTest, numericIdsAreRecordedCanonically) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", (int64_t)7, "n_i", 1), UpdateMessage::COMMIT).success);
  auto info = readDurableIndexInfo(iw->dir);
  ASSERT_EQ(1u, info->segments.size());
  const auto& segment = info->segments[0];
  SourceReader reader(iw->dir, Postings::getSourceMapFileName(Postings::getSortableString(segment.seg_id),
                                                              segment.source_gen), segment.max_doc);
  SourceReader::Cursor cursor;
  source::Record record;
  ASSERT_EQ(SourceReader::Status::OK, reader.fetch(0, cursor, record));
  EXPECT_EQ("7", record.id);
  EXPECT_EQ(source::kFlagProtoMap, record.flags & source::kFlagProtoMap);  // a Map, not JSON text
  EXPECT_EQ((std::vector<std::string>{R"({"id":7,"n_i":1})"}), sources(helper));
}

TEST_F(SourceStoreTest, unreadableMapFailsTheMergeAndDamagedFilesAreCarried) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  auto info = readDurableIndexInfo(iw->dir);
  ASSERT_EQ(2u, info->segments.size());
  auto mapName = Postings::getSourceMapFileName(Postings::getSortableString(info->segments[0].seg_id),
                                                info->segments[0].source_gen);
  auto files = sourceFileNames(*info);
  ASSERT_EQ(2u, files.size());

  // A damaged map: the merge cannot carry the segment's references, so it fails.
  damage(iw->dir, mapName);
  EXPECT_FALSE(forceMerge(helper).success);
  EXPECT_EQ(2u, helper.durableSegmentCount());
}

TEST_F(SourceStoreTest, damagedSourceFileStaysReferencedAndCostsOnlyItsDocs) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  enableSource(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  auto files = sourceFileNames(*readDurableIndexInfo(iw->dir));
  ASSERT_EQ(2u, files.size());
  damage(iw->dir, files[0]);
  // Default thresholds would relocate both tiny files; the damaged one cannot be
  // verified, so the merge keeps referencing it instead of dropping its docs.
  ASSERT_TRUE(forceMerge(helper).success);
  auto after = sourceFileNames(*readDurableIndexInfo(iw->dir));
  EXPECT_TRUE(std::ranges::contains(after, files[0])) << files[0];
  auto got = sources(helper);
  ASSERT_EQ(3u, got.size());
  EXPECT_EQ("", got[0]);
  EXPECT_EQ("", got[1]);
  EXPECT_EQ(R"({"id":"c"})", got[2]);
}

TEST_F(SourceStoreTest, enabledFalseStopsCaptureAndReplaceAllKeepsTheEntry) {
  CollectionHelper helper;
  enableSource(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  enableSource(helper, [](Resource& r) { r.enabled = false; });
  EXPECT_FALSE(helper.collection().getSchema()->source.enabled);
  ASSERT_TRUE(helper.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a"})", ""}), sources(helper));

  // A replacement that says nothing about _source_ keeps the current entry.
  SchemaBuilder b;
  b.field("title_t").type = api::FieldDef::FieldClass::TEXT;
  auto replaced = b.replaceAll(helper.collection());
  EXPECT_FALSE(replaced->source.enabled);
  EXPECT_NE(std::string::npos, replaced->sourceDef_.find("_source_"));
  // One that names it wins.
  SchemaBuilder c;
  c.storedResource(Postings::SOURCE_RESOURCE);
  EXPECT_TRUE(c.replaceAll(helper.collection())->source.enabled);
}

TEST_F(SourceStoreTest, includesAndExcludesNarrowMapsToo) {
  CollectionHelper helper;
  enableSource(helper, [](Resource& r) {
    static const std::string_view patterns[] = {"secret_s"};
    r.excludes = patterns;
  });
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "secret_s", "k", "name_s", "n"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a","name_s":"n"})"}), sources(helper));
  // The dropped field is still indexed.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("secret_s", "k").fields({"id"});
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(1u, req->getDocs().size());
}

TEST_F(SourceStoreTest, requestingSourceWithoutTheStoreIsAnError) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"_source"});
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("invalid_field", req->errorCode());
}

TEST_F(SourceStoreTest, overwriteThenMergeKeepsTheLiveDocsSources) {
  CollectionHelper helper;
  enableSource(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "n_i", 5), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 2), UpdateMessage::COMMIT, true).success);  // overwrites a
  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(forceMerge(helper).success);
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a","n_i":2})"}), sources(helper));
}

TEST_F(SourceStoreTest, disableThenEnableCapturesOnlyWhileOn) {
  CollectionHelper helper;
  enableSource(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  enableSource(helper, [](Resource& r) { r.enabled = false; });
  ASSERT_TRUE(helper.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  enableSource(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a"})", "", R"({"id":"c"})"}), sources(helper));
}

TEST_F(SourceStoreTest, lz4SourcesSurviveAMerge) {
  CollectionHelper helper;
  enableSource(helper, [](Resource& r) { r.codec = Resource::Codec::LZ4; r.min_file_bytes = 0; });
  const std::string big(5000, 'x');
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "title_t", big)}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(forceMerge(helper).success);
  EXPECT_EQ((std::vector<std::string>{R"({"id":"a","title_t":")" + big + R"("})", R"({"id":"b"})"}), sources(helper));
}

TEST_F(SourceStoreTest, schemaRejectsMisuse) {
  auto rejects = [](const std::function<void(SchemaBuilder&)>& define) {
    SchemaBuilder b;
    define(b);
    EXPECT_THROW(b.build(), SchemaError);
  };
  rejects([](SchemaBuilder& b) { b.storedResource("_source_").layout = Resource::Layout::INLINE; });
  rejects([](SchemaBuilder& b) { b.storedResource("_stored_").layout = Resource::Layout::SEPARATED; });
  rejects([](SchemaBuilder& b) { b.storedResource("_stored_").enabled = false; });
  rejects([](SchemaBuilder& b) { b.storedResource("_stored_").max_files = 4; });
  rejects([](SchemaBuilder& b) { b.storedResource("_source_").zstd_dictionary = true; });
  rejects([](SchemaBuilder& b) { b.field("_source_").type = api::FieldDef::FieldClass::TEXT; });
  rejects([](SchemaBuilder& b) { b.storedResource("_source_").max_files = 0; });
  rejects([](SchemaBuilder& b) { b.storedResource("_source_").min_live_ratio = 0.0; });
  rejects([](SchemaBuilder& b) {
    static const std::string_view in[] = {"a"}, out[] = {"b"};
    auto& r = b.storedResource("_source_");
    r.includes = in;
    r.excludes = out;
  });
  rejects([](SchemaBuilder& b) {
    b.storedResource("_source_");
    auto& f = b.field("body_t");
    f.type = api::FieldDef::FieldClass::TEXT;
    f.stored = true;
    f.stored_resource = "_source_";
  });
  // Accepted: the defaults, lz4, and a block doc cap.
  SchemaBuilder ok;
  auto& r = ok.storedResource("_source_");
  r.codec = Resource::Codec::LZ4;
  r.max_docs_per_chunk = 8;
  auto schema = ok.build();
  EXPECT_TRUE(schema->source.enabled);
  EXPECT_EQ(source::Codec::LZ4, schema->source.codec);
  EXPECT_EQ(8u, schema->source.maxDocsPerBlock);
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("_source_"));
}

}  // namespace luxir::test
