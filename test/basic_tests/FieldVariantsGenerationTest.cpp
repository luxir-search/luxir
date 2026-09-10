// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include <array>
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

  static std::shared_ptr<Schema> put(Collection& collection, std::string_view json,
                                   api::SchemaRequest_::Mode mode = api::SchemaRequest_::Mode::SET) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
    return collection.updateSchema(def, mode);
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

  static void rejected(Collection& collection, std::string_view json,
                       std::string_view field, std::string_view property) {
    auto before = collection.getSchema();
    try {
      put(collection, json);
      FAIL() << "Incompatible schema accepted";
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find(field)) << e.what();
      EXPECT_NE(std::string::npos, std::string(e.what()).find(property)) << e.what();
      EXPECT_NE(std::string::npos, std::string(e.what()).find("reindex")) << e.what();
    }
    EXPECT_EQ(before, collection.getSchema());
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
    rejected(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"text"}}}})",
             "author__s", "type");
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
  void finish() { unpause(); finished.wait(); }
};

// Observe real publication/admission transitions without timing sleeps or
// blocking the writer mutex in the listener.
class SchemaPublicationProbe {
  IndexWriter& writer;
  std::promise<void> quiescingPromise;
  std::promise<void> admissionPromise;
  std::future<void> quiescing = quiescingPromise.get_future();
  std::future<void> admission = admissionPromise.get_future();
  std::atomic<bool> sawQuiesce = false;
  std::atomic<int> admissionWaits = 0;
public:
  explicit SchemaPublicationProbe(IndexWriter& writer) : writer(writer) {
    Signal::listen("schemaQuiesce", [this](void* source, void*, void*) -> void* {
      if (source == &this->writer && !sawQuiesce.exchange(true)) quiescingPromise.set_value();
      return nullptr;
    });
    Signal::listen("schemaAdmissionWait", [this](void* source, void*, void*) -> void* {
      if (source == &this->writer && admissionWaits.fetch_add(1) == 0) admissionPromise.set_value();
      return nullptr;
    });
  }
  ~SchemaPublicationProbe() {
    Signal::unlisten("schemaQuiesce");
    Signal::unlisten("schemaAdmissionWait");
  }
  int waitingAdmissions() const { return admissionWaits.load(); }
  bool waitQuiescing() { return quiescing.wait_for(std::chrono::seconds(5)) == std::future_status::ready; }
  bool waitAdmission() { return admission.wait_for(std::chrono::seconds(5)) == std::future_status::ready; }
};

TEST_F(FieldVariantsGenerationTest, admissionPinsBeforePublicationEvenWhenAcquisitionIsDelayed) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"fields":{"author":"text"}})");
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old", "author", "Le Guin"));
  PausedSchemaUpdate paused(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&paused));
  paused.entered.wait();
  EXPECT_EQ(before, paused.schema);
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"string"}}}})");
  });
  bool ready = publication.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  EXPECT_TRUE(ready) << "A pure addition must not wait for the old message";
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
  auto durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(2u, durable->segments.size());
  EXPECT_NE(durable->segments[0].schema_gen, durable->segments[1].schema_gen);
}

TEST_F(FieldVariantsGenerationTest, templateEditQuiescesUnrelatedMessageAndBlocksNewAdmission) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"templates":{"_edit":{"type":"text",
    "analyzer":{"tokenizer":"whitespace","filters":["lowercase"]}}}})");
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old", "keep_w", "Old Word"));
  PausedSchemaUpdate old(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&old));
  old.entered.wait();
  SchemaPublicationProbe probe(*writer);
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"templates":{"_edit":{"type":"text",
      "analyzer":{"tokenizer":"keyword","filters":["lowercase"]}}}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  EXPECT_EQ(std::future_status::timeout, publication.wait_for(std::chrono::seconds(0)));
  CollectionHelper::UpdateBuilder newDocs;
  newDocs.add(flatdoc("id", "new", "city_edit", "New Word"));
  PausedSchemaUpdate next(newDocs.finish());
  EXPECT_TRUE(writer->submitUpdate(&next));
  EXPECT_TRUE(probe.waitAdmission());
  old.finish();
  auto after = publication.get();
  next.entered.wait();
  EXPECT_EQ(before, old.schema);
  EXPECT_EQ(after, next.schema);
  next.finish();
  EXPECT_FALSE(old.result.errored()) << old.result.what();
  EXPECT_FALSE(next.result.errored()) << next.result.what();
  helper.commit();
  EXPECT_EQ((std::vector<std::string>{"old"}), hits(helper, "keep_w:Word"));
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "city_edit:=\"new word\""));
  auto durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(2u, durable->segments.size());
  EXPECT_EQ(before->gen_, durable->segments[0].schema_gen);
  EXPECT_EQ(after->gen_, durable->segments[1].schema_gen);
}

TEST_F(FieldVariantsGenerationTest, templateEditRejectsRootMaterializedDuringQuiesce) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"templates":{"_edit":{"type":"text",
    "analyzer":{"tokenizer":"whitespace","filters":["lowercase"]}}}})");
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old", "city_edit", "Old Word"));
  PausedSchemaUpdate old(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&old));
  old.entered.wait();
  SchemaPublicationProbe probe(*writer);
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"templates":{"_edit":{"type":"text",
      "analyzer":{"tokenizer":"keyword","filters":["lowercase"]}}}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  EXPECT_EQ(std::future_status::timeout, publication.wait_for(std::chrono::seconds(0)));
  old.finish();
  try {
    publication.get();
    FAIL() << "The drained message materialized an incompatible root";
  } catch (const SchemaError& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("city_edit"));
    EXPECT_NE(std::string::npos, std::string(e.what()).find("analyzer"));
  }
  EXPECT_EQ(before, helper.collection().getSchema());
  EXPECT_FALSE(old.result.errored()) << old.result.what();
  // Rejection reopens admission and leaves the old analysis intact.
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "city_edit", "New Word"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"new", "old"}), hits(helper, "city_edit:word"));
  auto durable = readDurableIndexInfo(writer->dir);
  for (const auto& segment : durable->segments) EXPECT_EQ(before->gen_, segment.schema_gen);
}

TEST_F(FieldVariantsGenerationTest, unusedFieldTypeChangeWaitsForUnrelatedIngest) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"fields":{"mistake":"string"}})");
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old", "keep_w", "old"));
  PausedSchemaUpdate old(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&old));
  old.entered.wait();
  SchemaPublicationProbe probe(*writer);
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"fields":{"mistake":"int"}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  old.finish();
  auto after = publication.get();
  EXPECT_EQ(before, old.schema);
  EXPECT_NE(before, after);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "mistake", 12), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"old"}), hits(helper, "keep_w:old"));
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "mistake:=12"));
}

TEST_F(FieldVariantsGenerationTest, directClientsParticipateInQuiesceAndAdmissionGate) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = helper.collection().getSchema();
  auto& busy = writer->obtainInverter();
  SchemaPublicationProbe probe(*writer);
  auto publication = std::async(std::launch::async, [&] {
    // This explicit name previously resolved through the default _s template.
    return put(helper.collection(), R"({"fields":{"count_s":"int"}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  EXPECT_EQ(std::future_status::timeout, publication.wait_for(std::chrono::seconds(0)));
  auto acquisition = std::async(std::launch::async, [&] {
    auto& next = writer->obtainInverter();
    auto schema = next.schema;
    writer->releaseInverter(next);
    return schema;
  });
  EXPECT_TRUE(probe.waitAdmission());
  busy.startDoc();
  busy.getIndexHandler("id").index(busy, "old");
  busy.finishDoc();
  writer->releaseInverter(busy);
  auto after = publication.get();
  EXPECT_NE(before, after);
  EXPECT_EQ(after, acquisition.get());
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "count_s", 12), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "count_s:=12"));
}

TEST_F(FieldVariantsGenerationTest, closeAbortsPublicationAndBlockedAdmission) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"fields":{"mistake":"string"}})");
  CollectionHelper::UpdateBuilder docs;
  docs.add(flatdoc("id", "old"));
  PausedSchemaUpdate old(docs.finish());
  ASSERT_TRUE(writer->submitUpdate(&old));
  old.entered.wait();
  SchemaPublicationProbe probe(*writer);
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"fields":{"mistake":"int"}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  PausedSchemaUpdate next(docs.finish());
  EXPECT_TRUE(writer->submitUpdate(&next));
  EXPECT_TRUE(probe.waitAdmission());
  auto closing = std::async(std::launch::async, [&] { writer->close(); });
  bool aborted = publication.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  EXPECT_TRUE(aborted) << "Close must abort publication before the old message finishes";
  if (!aborted) old.unpause();
  EXPECT_THROW(publication.get(), IndexWriterClosedError);
  next.finish();
  EXPECT_TRUE(next.result.errored());
  EXPECT_EQ(0u, next.updateVersion);
  EXPECT_EQ(nullptr, next.schema);
  EXPECT_EQ(before, helper.collection().getSchema());
  old.finish();
  closing.get();
  EXPECT_FALSE(old.result.errored()) << old.result.what();
}

TEST_F(FieldVariantsGenerationTest, serialAdmissionGateHoldsQueuedMessagesUntilPublication) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  auto before = put(helper.collection(), R"({"fields":{"mistake":"string"}})");
  auto& busy = writer->obtainInverter();
  SchemaPublicationProbe probe(*writer);
  // The publisher is a request thread, outside the update graph.
  auto publication = std::async(std::launch::async, [&] {
    return put(helper.collection(), R"({"fields":{"mistake":"int"}})");
  });
  EXPECT_TRUE(probe.waitQuiescing());
  std::array<CollectionHelper::UpdateBuilder, 4> docs;
  std::vector<std::unique_ptr<PausedSchemaUpdate>> messages;
  for (int i = 0; i < (int)docs.size(); ++i) {
    docs[i].add(flatdoc("id", std::to_string(i), "mistake", 12));
    auto message = std::make_unique<PausedSchemaUpdate>(docs[i].finish());
    message->unpause();
    EXPECT_TRUE(writer->submitUpdate(message.get()));
    messages.push_back(std::move(message));
  }
  EXPECT_TRUE(probe.waitAdmission());
  // Concurrency 1 leaves the remaining messages queued behind the one waiter.
  EXPECT_EQ(1, probe.waitingAdmissions());
  writer->releaseInverter(busy);
  auto after = publication.get();
  EXPECT_NE(before, after);
  for (auto& message : messages) {
    message->finish();
    EXPECT_EQ(after, message->schema);
    EXPECT_FALSE(message->result.errored()) << message->result.what();
  }
  EXPECT_EQ(1, probe.waitingAdmissions());
  helper.commit();
  EXPECT_EQ((std::vector<std::string>{"0", "1", "2", "3"}), hits(helper, "mistake:=12"));
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
  writer->releaseInverter(old);
  auto& fresh = writer->obtainInverter();
  EXPECT_EQ(after, fresh.schema);
  EXPECT_NE(oldSegment, fresh.postingsWriter.getSegId());
  writer->releaseInverter(fresh);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "author", "Le Guin"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author__s:*"));
  EXPECT_EQ((std::vector<std::string>{"also_old", "new", "old"}), hits(helper, "author:*"));
}

TEST_F(FieldVariantsGenerationTest, removedSignaturesAndMaterializedTemplatesSurviveRestart) {
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    put(helper.collection(), R"({"fields":{"author":{"type":"text","variants":{"s":"string"}}},
      "templates":{"_name":{"type":"text","analyzer":{"tokenizer":"whitespace","filters":["lowercase"]}}}})");
    ASSERT_TRUE(helper.index(flatdoc("id", "a", "author", "Le Guin", "city_name", "New York"), UpdateMessage::COMMIT).success);
    put(helper.collection(), R"({"fields":{"author":"text"}})");
    // Reopen the newer schema alongside the existing commit manifest.
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto& collection = helper.collection();
    auto durable = readDurableIndexInfo(helper.getIndexWriter()->dir);
    bool found = false;
    for (const auto& signature : durable->field_signatures) {
      if (signature.name != "author__s") continue;
      found = true;
      EXPECT_EQ("author", signature.logical_name);
      EXPECT_EQ("s", signature.label);
      EXPECT_EQ("\"string\"", signature.properties.at("type"));
    }
    EXPECT_TRUE(found);
    rejected(collection, R"({"fields":{"author":{"type":"text","variants":{"s":"text"}}}})", "author__s", "type");
    rejected(collection, R"({"fields":{"author":{"type":"text","variants":{"s":{"type":"string","normalizer":["nfkc_cf"]}}}}})",
             "author__s", "normalizer");
    rejected(collection, R"({"templates":{"_name":{"type":"text","analyzer":{"tokenizer":"keyword","filters":["lowercase"]}}}})",
             "city_name", "analyzer");
    // Equivalent effective analysis through inheritance and shorthand is canonical.
    EXPECT_NO_THROW(put(collection, R"({"templates":{"_base_":{"type":"text","analyzer":{"tokenizer":{"name":"whitespace"},"filters":[{"name":"lowercase"}]}},"_name":{"parent":"_base_","variants":{"s":"string"}},"_new":"int"},
      "fields":{"author":{"type":"text","variants":{"s":"string","folded":{"type":"string","normalizer":["nfkc_cf"]}}},"extra":"int"}})"));
    auto resolved = view(*helper.getIndexWriter());
    auto& readded = resolved["fields"]["author"]["representations"]["s"];
    EXPECT_EQ(collection.getSchema()->gen_, readded["introduced_generation"].get<uint64_t>());
    EXPECT_FALSE(readded["coverage_complete"].get<bool>());
    ASSERT_TRUE(helper.index(flatdoc("id", "b", "city_name", "New York", "count_new", 5), UpdateMessage::COMMIT).success);
    EXPECT_EQ((std::vector<std::string>{"b"}), hits(helper, "city_name__s:*"));
    rejected(collection, R"({"fields":{"city_name":{"type":"text","analyzer":{"tokenizer":"keyword"}}}})", "city_name", "analyzer");
  }
}

TEST_F(FieldVariantsGenerationTest, signaturesRejectShapeIndexColumnAndVectorChanges) {
  LuxirNode node;
  CollectionHelper helper(node);
  put(helper.collection(), R"({"fields":{"tag":"string","count":{"type":"int","index":"range"},"vec":{"type":"vector","dims":2,"metric":"l2"}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "tag", "x", "count", 1, "vec", std::vector<float>{1, 2}), UpdateMessage::COMMIT).success);
  for (auto json : {R"({"fields":{"tag":{"type":"string","multi":true}}})",
                    R"({"fields":{"tag":{"type":"string","column":false}}})",
                    R"({"fields":{"count":{"type":"int","index":"none"}}})",
                    R"({"fields":{"vec":{"type":"vector","dims":3,"metric":"l2"}}})",
                    R"({"fields":{"vec":{"type":"vector","dims":2,"metric":"ip"}}})"}) {
    EXPECT_THROW(put(helper.collection(), json), SchemaError) << json;
  }
  // Storage and binding choices are not a dictionary/column reinterpretation.
  EXPECT_NO_THROW(put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true,"variants":{"text":"text"},"defaults":{"search":"text"}}}})"));
}

TEST_F(FieldVariantsGenerationTest, idleInverterDoesNotReserveUnusedTemplateInstances) {
  LuxirNode node;
  CollectionHelper helper(node);
  ASSERT_TRUE(helper.index(flatdoc("id", "old")).success);
  EXPECT_NO_THROW(put(helper.collection(), R"({"fields":{"count_s":"int"}})"));
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "count_s", 12), UpdateMessage::COMMIT).success);
  EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "count_s:=12"));
}

TEST_F(FieldVariantsGenerationTest, bufferedDynamicRootReservesItsSignatureBeforeFirstFlush) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  ASSERT_TRUE(helper.index(flatdoc("id", "old", "city_t", "New York")).success);
  for (bool flushed : {false, true}) {
    if (flushed) helper.commit();
    CollectionHelper::UpdateBuilder docs;
    docs.add(flatdoc("id", flushed ? "after_flush" : "buffered"));
    PausedSchemaUpdate old(docs.finish());
    ASSERT_TRUE(writer->submitUpdate(&old));
    old.entered.wait();
    auto publication = std::async(std::launch::async, [&] {
      rejected(helper.collection(), R"({"templates":{"_t":{"type":"text","analyzer":{"tokenizer":"keyword"}}}})",
               "city_t", "analyzer");
    });
    bool ready = publication.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    EXPECT_TRUE(ready) << "Known materialized conflicts must reject before draining";
    if (!ready) old.unpause();
    publication.get();
    old.finish();
  }
  helper.commit();
  EXPECT_EQ((std::vector<std::string>{"old"}), hits(helper, "city_t:York"));
}

TEST_F(FieldVariantsGenerationTest, explicitRootRemovalIntroducesPreviouslySuppressedTemplateVariant) {
  uint64_t introduced;
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    put(helper.collection(), R"({"fields":{"author_t":"text"},
      "templates":{"_t":{"type":"text","variants":{"s":"string"}}}})");
    ASSERT_TRUE(helper.index(flatdoc("id", "old", "author_t", "Le Guin")).success);
    introduced = put(helper.collection(), R"({"templates":{"_t":{"type":"text","variants":{"s":"string"}}}})",
                     api::SchemaRequest_::Mode::REPLACE_ALL)->gen_;
    ASSERT_TRUE(helper.index(flatdoc("id", "new", "author_t", "Le Guin"), UpdateMessage::COMMIT).success);
    EXPECT_EQ((std::vector<std::string>{"new"}), hits(helper, "author_t__s:*"));
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto json = view(*helper.getIndexWriter());
    auto& rep = json["fields"]["author_t"]["representations"]["s"];
    EXPECT_EQ(introduced, rep["introduced_generation"].get<uint64_t>());
    EXPECT_FALSE(rep["coverage_complete"].get<bool>());
  }
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

TEST_F(FieldVariantsGenerationTest, longTermsPolicyChangesPreserveSignaturesAndStoredSource) {
  std::string source(300, 'x');
  auto checkSource = [&](CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("id", "old").fields({"tag"});
    req->execute();
    EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("tag", source)));
  };
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    auto before = put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true}}})");
    ASSERT_TRUE(helper.index(flatdoc("id", "old", "tag", source), UpdateMessage::COMMIT).success);
    checkSource(helper);
    auto after = put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true,"long_terms":"reject"}}})");
    EXPECT_EQ(before->physical("tag")->segmentFlags(), after->physical("tag")->segmentFlags());
    EXPECT_EQ(before->signatures().at("tag").properties, after->signatures().at("tag").properties);
    checkSource(helper); // A current reject policy cannot make the old column source-equivalent.
    EXPECT_FALSE(helper.index(flatdoc("id", "bad", "tag", source)).success);
    ASSERT_TRUE(helper.index(flatdoc("id", "new", "tag", "short"), UpdateMessage::COMMIT).success);
  }
  {
    LuxirNode node(config());
    CollectionHelper helper(node);
    EXPECT_TRUE(helper.collection().getSchema()->physical("tag")->rejectLongTerms);
    checkSource(helper);
    auto durable = readDurableIndexInfo(helper.getIndexWriter()->dir);
    for (const auto& signature : durable->field_signatures) EXPECT_FALSE(signature.properties.contains("long_terms"));
    put(helper.collection(), R"({"fields":{"tag":{"type":"string","stored":true,"long_terms":"truncate"}}})");
    ASSERT_TRUE(helper.index(flatdoc("id", "again", "tag", source), UpdateMessage::COMMIT).success);
    EXPECT_EQ((std::vector<std::string>{"again", "old"}), hits(helper, "tag:=" + source));
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(helper.submit(merge).success);
    checkSource(helper);
  }
}

} // namespace luxir::test
