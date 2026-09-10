// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include <filesystem>
#include <future>
#include <latch>

namespace luxir::test {

class FieldVariantsGenerationTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("luxir-schema-generations-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  using Json = glz::generic_u64;

  void TearDown() override { std::filesystem::remove_all(path); }

  LuxirConfig config() {
    LuxirConfig config;
    config.store.backend = "fs";
    config.store.data_dir = path.string();
    return config;
  }

  static std::shared_ptr<Schema> put(Collection& collection, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
    return collection.updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  static Json view(IndexWriter& writer) {
    Json value;
    if (glz::read_json(value, writer.resolvedSchema())) throw std::runtime_error("Invalid resolved view");
    return value;
  }

  static std::vector<std::string> hits(CollectionHelper& helper, std::string_view expr) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expr).fields({"id"}).limit(-1);
    req->execute();
    std::vector<std::string> ids;
    for (const auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

TEST_F(FieldVariantsGenerationTest, cachedRootVariantCoverageSurvivesReopenAndMerge) {
  uint64_t before, introduced;
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto& collection = helper.collection();
    before = put(collection, R"({"fields":{"author":"text"}})")->gen_;
    ASSERT_TRUE(helper.index(flatdoc("id", "old", "author", "Le Guin")).success);
    auto writer = helper.getIndexWriter();
    auto& cached = writer->obtainInverter();
    EXPECT_TRUE(cached.inputHandlers.contains("author"));
    writer->releaseInverter(cached);
    introduced = put(collection, R"({"fields":{"author":{"type":"text",
      "variants":{"s":{"type":"string","normalizer":["nfkc_cf"]}},"defaults":{"value":"s"}}}})")->gen_;
    ASSERT_TRUE(helper.index(flatdoc("id", "new", "author", "Le Guin"), UpdateMessage::COMMIT).success);
    EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author:=\"LE GUIN\""));
    EXPECT_EQ((std::vector<std::string>{"new", "old"}), hits(helper, "author:Guin"));
    auto durable = readDurableIndexInfo(writer->dir);
    ASSERT_EQ(2u, durable->segments.size());
    std::vector<uint64_t> generations;
    for (const auto& segment : durable->segments) generations.push_back(segment.schema_gen);
    std::sort(generations.begin(), generations.end());
    EXPECT_EQ((std::vector<uint64_t>{before, introduced}), generations);
    auto json = view(*writer);
    auto& rep = json["fields"]["author"]["representations"]["s"];
    EXPECT_EQ(before, rep["oldest_generation"].get<uint64_t>());
    EXPECT_EQ(introduced, rep["introduced_generation"].get<uint64_t>());
    EXPECT_FALSE(rep["coverage_complete"].get<bool>());
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto writer = helper.getIndexWriter();
    auto json = view(*writer);
    auto& rep = json["fields"]["author"]["representations"]["s"];
    EXPECT_EQ(before, rep["oldest_generation"].get<uint64_t>());
    EXPECT_EQ(introduced, rep["introduced_generation"].get<uint64_t>());
    EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author__s:=\"le guin\""));
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(helper.submit(merge).success);
    auto durable = readDurableIndexInfo(writer->dir);
    ASSERT_EQ(1u, durable->segments.size());
    EXPECT_EQ(before, durable->segments[0].schema_gen);
    EXPECT_FALSE(view(*writer)["fields"]["author"]["representations"]["s"]["coverage_complete"].get<bool>());
    EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author__s:=\"le guin\""));
    ASSERT_TRUE(helper.deleteById("old", UpdateMessage::COMMIT).success);
    // Delete + overwrite is real reindexing; merging alone did not fill the variant.
    ASSERT_TRUE(helper.index(flatdoc("id", "new", "author", "Le Guin"), UpdateMessage::COMMIT, true).success);
    EXPECT_TRUE(view(*writer)["fields"]["author"]["representations"]["s"]["coverage_complete"].get<bool>());
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    EXPECT_EQ(introduced, readDurableIndexInfo(helper.getIndexWriter()->dir)->segments[0].schema_gen);
  }
}

// Pause after admission, before ProtoUpdateMessage obtains an inverter. This
// explicitly covers a queued old message obtaining its inverter AFTER publication.
class PausedSchemaUpdate : public ProtoUpdateMessage {
public:
  std::latch entered{1};
  std::latch resume{1};
  std::latch finished{1};
  explicit PausedSchemaUpdate(const api::UpdateRequest& request) : ProtoUpdateMessage(&request) {}
  void handle(IndexWriter& writer) override {
    entered.count_down();
    resume.wait();
    ProtoUpdateMessage::handle(writer);
  }
  void done(IndexWriter&) override { finished.count_down(); }
  void unpause() { if (!resume.try_wait()) resume.count_down(); }
};

TEST_F(FieldVariantsGenerationTest, admissionPinsBeforeSchemaEditEvenWhenAcquisitionIsDelayed) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  SchemaBuilder b;
  auto& author = b.field("author");
  author.type = api::FieldDef::FieldClass::TEXT;
  b.analyzer(author, "whitespace", {"lowercase"});
  auto before = b.set(helper.collection());
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old", "author", "Le Guin"));
  PausedSchemaUpdate paused(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&paused));
  paused.entered.wait();
  EXPECT_EQ(before, paused.schema);
  auto publication = std::async(std::launch::async, [&] {
    b.analyzer(author, "keyword", {"lowercase"});
    b.variant(author, "s").type = api::FieldDef::FieldClass::STRING;
    return b.set(helper.collection());
  });
  bool ready = publication.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  EXPECT_TRUE(ready) << "A schema edit must not wait for the old message";
  if (!ready) paused.unpause();
  auto after = publication.get();
  // The next message is admitted while the old message is still paused.
  CollectionHelper::UpdateBuilder newDocs;
  newDocs.add(flatdoc("id", "new", "author", "Le Guin"));
  PausedSchemaUpdate next(newDocs.finish());
  EXPECT_TRUE(writer->submitUpdate(&next));
  next.entered.wait();
  EXPECT_EQ(after, next.schema);
  next.unpause();
  paused.unpause();
  paused.finished.wait();
  next.finished.wait();
  EXPECT_FALSE(paused.result.errored()) << paused.result.what();
  EXPECT_FALSE(next.result.errored()) << next.result.what();
  helper.commit();
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author__s:*"));
  EXPECT_EQ((std::vector<std::string>{"old"}), hits(helper, "author:=guin"));
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author:=\"le guin\""));
  auto durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(2u, durable->segments.size());
  EXPECT_NE(durable->segments[0].schema_gen, durable->segments[1].schema_gen);
}

TEST_F(FieldVariantsGenerationTest, busyOldInverterFinishesAndRetiresWithoutRefreshingUnknownNames) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"fields":{"author":"text"}})");
  auto& old = writer->obtainInverter();
  auto oldSegment = old.postingsWriter.getSegId();
  auto& author = old.getIndexHandler("author");
  old.startDoc();
  old.getIndexHandler("id").index(old, "old");
  author.index(old, "Le Guin");
  auto after = put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"string"}},"added":"string"}})");
  EXPECT_EQ(before, old.schema);
  EXPECT_THROW(old.getIndexHandler("added"), RequestError);
  old.finishDoc();
  old.startDoc();
  old.getIndexHandler("id").index(old, "also_old");
  author.index(old, "Le Guin");
  old.finishDoc();
  std::promise<void> flushing;
  auto started = flushing.get_future();
  Signal::listen("segmentFlushBody", [&](void* source, void*, void*) -> void* {
    if (((Inverter*)source)->postingsWriter.getSegId() == oldSegment) flushing.set_value();
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("segmentFlushBody"); });
  writer->releaseInverter(old);
  EXPECT_EQ(std::future_status::ready, started.wait_for(std::chrono::seconds(5)));
  auto& fresh = writer->obtainInverter();
  EXPECT_EQ(after, fresh.schema);
  EXPECT_NE(oldSegment, fresh.postingsWriter.getSegId());
  writer->releaseInverter(fresh);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "author", "Le Guin"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author__s:*"));
  EXPECT_EQ((std::vector<std::string>{"also_old", "new", "old"}), hits(helper, "author:*"));
}

TEST_F(FieldVariantsGenerationTest, removedVariantAndMaterializedTemplateCanBeRedefinedAfterReopen) {
  uint64_t before;
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    before = put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"string"}}},
      "templates":{"_name":{"type":"text","analyzer":{"tokenizer":"whitespace"}}}})")->gen_;
    ASSERT_TRUE(helper.index(flatdoc("id", "old", "author", "Le Guin", "city_name", "New York"),
                             UpdateMessage::COMMIT).success);
    put(helper.collection(), R"({"fields":{"author":"text"}})");
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto after = put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"text"}}},
      "templates":{"_name":{"type":"text","analyzer":{"tokenizer":"keyword"}}}})");
    ASSERT_TRUE(helper.index(flatdoc("id", "new", "author", "Le Guin", "city_name", "New York"),
                             UpdateMessage::COMMIT).success);
    auto writer = helper.getIndexWriter();
    auto json = view(*writer);
    auto& rep = json["fields"]["author"]["representations"]["s"];
    EXPECT_EQ("text", rep["type"].get<std::string>());
    EXPECT_EQ(after->gen_, rep["introduced_generation"].get<uint64_t>());
    EXPECT_EQ(before, rep["oldest_generation"].get<uint64_t>());
    EXPECT_FALSE(rep["coverage_complete"].get<bool>());
    EXPECT_FALSE(json["fields"].get<Json::object_t>().contains("city_name"));
    auto durable = readDurableIndexInfo(writer->dir);
    ASSERT_EQ(2u, durable->segments.size());
    EXPECT_EQ(before, durable->segments[0].schema_gen);
    EXPECT_EQ(after->gen_, durable->segments[1].schema_gen);
  }
}

TEST_F(FieldVariantsGenerationTest, staleIdleInverterFlushesOnCheckoutEvenWithAnOldPin) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  auto before = helper.collection().getSchema();
  ASSERT_TRUE(helper.index(flatdoc("id", "old")).success);
  auto& idle = writer->obtainInverter();
  auto oldSegment = idle.postingsWriter.getSegId();
  writer->releaseInverter(idle);

  SchemaBuilder b;
  b.field("added").type = api::FieldDef::FieldClass::STRING;
  auto after = b.set(helper.collection());
  std::promise<void> flushing;
  auto started = flushing.get_future();
  Signal::listen("segmentFlushBody", [&](void* source, void*, void*) -> void* {
    if (((Inverter*)source)->postingsWriter.getSegId() == oldSegment) flushing.set_value();
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("segmentFlushBody"); });
  auto& pinned = writer->obtainInverter(0, before);
  EXPECT_EQ(before, pinned.schema);
  EXPECT_NE(oldSegment, pinned.postingsWriter.getSegId());
  EXPECT_EQ(std::future_status::ready, started.wait_for(std::chrono::seconds(5)));
  writer->releaseInverter(pinned);
  auto& fresh = writer->obtainInverter();
  EXPECT_EQ(after, fresh.schema);
  writer->releaseInverter(fresh);
  helper.commit();
  auto durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, durable->segments.size());
  EXPECT_EQ(before->gen_, durable->segments[0].schema_gen);
}

TEST_F(FieldVariantsGenerationTest, schemaOnlyPublicationPreservesIntroductionsAndAuthoredHttpEcho) {
  uint64_t introduced;
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    introduced = put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"string"},"defaults":{"value":"s"}}}})")->gen_;
    put(helper.collection(), R"({"fields":{"later":"int"}})");
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    HttpServer server(node, 2, 0);
    server.start();
    auto response = httpRequest(server.getPort(), http::verb::get, "/collections/main/_schema?view=resolved");
    ASSERT_EQ(200, response.result_int()) << response.body();
    Json json;
    ASSERT_FALSE(glz::read_json(json, response.body()));
    auto& author = json["fields"]["author"];
    auto& rep = author["representations"]["s"];
    EXPECT_EQ("author__s", rep["name"].get<std::string>());
    EXPECT_EQ("author__s", author["bindings"]["value"].get<std::string>());
    EXPECT_EQ(introduced, rep["introduced_generation"].get<uint64_t>());
    EXPECT_TRUE(rep["oldest_generation"].is_null());
    EXPECT_TRUE(rep["coverage_complete"].get<bool>());
    EXPECT_EQ("string", rep["type"].get<std::string>());
    EXPECT_EQ("match", rep["index"].get<std::string>());
    EXPECT_TRUE(rep["column"].get<bool>());
    EXPECT_FALSE(rep["stored"].get<bool>());
    auto authored = httpRequest(server.getPort(), http::verb::get, "/collections/main/_schema");
    ASSERT_EQ(200, authored.result_int());
    EXPECT_EQ(std::string::npos, authored.body().find("representations"));
    EXPECT_EQ(std::string::npos, authored.body().find("introduced_generation"));
    EXPECT_EQ(400, httpRequest(server.getPort(), http::verb::get, "/collections/main/_schema?view=bad").result_int());
    server.shutdown();
  }
}

TEST_F(FieldVariantsGenerationTest, longTermsResolvedSettingsAndStoredSourceSurviveReopenAndMerge) {
  std::string source(300, 'x');
  auto checkSource = [&](CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("id", "old").fields({"tag"});
    req->execute();
    EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("tag", source)));
  };
  for (bool reopen : {false, true}) {
    LuxirNode node(config());
    CollectionHelper helper(node);
    if (!reopen) {
      put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true}}})");
      ASSERT_TRUE(helper.index(flatdoc("id", "old", "tag", source), UpdateMessage::COMMIT).success);
    }
    checkSource(helper);
    auto json = view(*helper.getIndexWriter());
    for (auto name : {"tag", "id"}) {
      EXPECT_EQ("hash128", json["fields"][name]["representations"]["self"]["long_terms"].get<std::string>());
    }
    // Making the implicit default explicit preserves the representation.
    put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true,"long_terms":"hash128"}}})");
    if (reopen) {
      ASSERT_TRUE(helper.index(flatdoc("id", "again", "tag", source), UpdateMessage::COMMIT).success);
      CollectionHelper::UpdateBuilder merge;
      merge.commit(true, 1);
      ASSERT_TRUE(helper.submit(merge).success);
      EXPECT_EQ((std::vector<std::string>{"again", "old"}), hits(helper, "tag:=" + source));
      checkSource(helper);
    }
  }
}

} // namespace luxir::test
