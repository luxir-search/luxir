// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/StorageTestUtils.h"
#include "test/CollectionHelper.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/util/Signal.h"

using namespace luxir;
using namespace luxir::test;

namespace {
std::vector<std::string> schemas(Directory& dir) {
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  std::vector<std::string> names;
  for (const auto& f : files) if (SchemaBinding::isFile(f.name)) names.push_back(f.name);
  std::ranges::sort(names);
  return names;
}
}

TEST(SchemaStorageTest, immutableFilesAreReferencedSharedAndRetired) {
  LuxirNode node;
  CollectionHelper h(node);
  auto w = h.getIndexWriter();
  auto& registry = w->snapshots;
  auto first = registry.acquire();
  auto file = first->schema->file;
  int written = 0;
  Signal::listen("publishWritten", [&](void*, void* name, void*) -> void* {
    if (((std::string_view*)name)->starts_with("schema_")) written++;
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("publishWritten"); });
  EXPECT_EQ((std::vector<std::string>{file.name}), schemas(w->dir));
  ASSERT_TRUE(w->commit());
  auto second = registry.snapshot();
  EXPECT_EQ(first->schema, second->schema);
  EXPECT_EQ(first->schema, registry.resolve(second->bytes)->schema);
  EXPECT_EQ((std::vector<std::string>{file.name}), schemas(w->dir));
  EXPECT_EQ(0, written);
  SchemaBuilder b; b.field("added").type = api::FieldDef::FieldClass::STRING;
  auto schema = b.set(h.collection());
  EXPECT_EQ(1, written);
  EXPECT_EQ(2u, schemas(w->dir).size());
  auto manifest = readDurableIndexInfo(w->dir);
  EXPECT_TRUE(manifest->schema->name.empty());
  EXPECT_EQ("t0", manifest->schema->scope);
  EXPECT_EQ(schema->file.name, manifest->schema_file.name);
  EXPECT_NE(file.name, schema->file.name);
  EXPECT_NE(std::ranges::find(registry.snapshot()->files, schema->file), registry.snapshot()->files.end());
  registry.testEvictOldest();
  EXPECT_FALSE(w->dir.openFile(file.name));
  EXPECT_EQ((std::vector<std::string>{schema->file.name}), schemas(w->dir));
  // Readers need their parsed schema, not its now-retired backing file.
  EXPECT_NE(nullptr, first->schema->getFieldTypePtr("id"));
  EXPECT_GT(node.collections().schemaBytes("t0"), schema->estimatedBytes());
}

TEST(SchemaStorageTest, cacheSeparatesLocationsAndReleasesTenantCharges) {
  auto cache = std::make_shared<SchemaCache>();
  RAMDir dir;
  CommitSnapshotRegistry registry(test::reserveIncarnation(dir), {}, CommitSnapshotRegistry::Clock::now, cache, "tenant");
  IndexWriter writer(registry);
  auto bytes = registry.snapshot()->bytes;
  auto schema = registry.snapshot()->schema;
  CommitSnapshotRegistry another(dir, {}, CommitSnapshotRegistry::Clock::now, cache, "tenant");
  another.openLocalSnapshot();
  EXPECT_EQ(schema, another.snapshot()->schema);
  EXPECT_EQ(schema->estimatedBytes(), cache->bytes());
  EXPECT_EQ(cache->bytes(), cache->tenantBytes("tenant"));
  EXPECT_EQ(0u, cache->tenantBytes("other"));
  RAMDir independent;
  CommitSnapshotRegistry elsewhere(test::reserveIncarnation(independent), {}, CommitSnapshotRegistry::Clock::now, cache, "tenant");
  IndexWriter other(elsewhere);
  EXPECT_NE(schema, elsewhere.snapshot()->schema);
  EXPECT_EQ(schema->estimatedBytes() + elsewhere.snapshot()->schema->estimatedBytes(), cache->bytes());
  auto held = cache->load(dir, *readDurableIndexInfo(dir));
  EXPECT_EQ(schema, held);
  {
    auto temporary = std::make_shared<SchemaCache>();
    auto parsed = temporary->load(dir, *readDurableIndexInfo(dir));
    EXPECT_GT(temporary->bytes(), 0u);
    parsed.reset();
    EXPECT_EQ(0u, temporary->bytes());
  }
}

TEST(SchemaStorageTest, failedChangesAndWriterEpochsCannotReuseSchemaNames) {
  RAMDir dir;
  CommitSnapshotRegistry registry(test::reserveIncarnation(dir));
  IndexWriter writer(registry);
  auto previous = registry.snapshot();
  Signal::listen("publishWritten", [&](void*, void* name, void*) -> void* {
    if (((std::string_view*)name)->starts_with("commit_")) throw std::runtime_error("failed manifest");
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("publishWritten"); });
  EXPECT_THROW(writer.setSchema(Schema::createDefaultSchema()), std::runtime_error);
  Signal::unlisten("publishWritten");
  auto abandoned = schemas(dir);
  EXPECT_EQ(2u, abandoned.size());
  writer.setSchema(Schema::createDefaultSchema());
  EXPECT_EQ(3u, registry.snapshot()->schema->binding.version.local);
  EXPECT_NE(previous->schema->file.name, registry.snapshot()->schema->file.name);
  CommitSnapshotRegistry replacement(dir);
  IndexWriter newer(replacement);
  newer.setSchema(Schema::createDefaultSchema());
  EXPECT_EQ(2u, replacement.snapshot()->schema->binding.version.epoch);
  EXPECT_EQ(1u, schemas(dir).size()); // older-epoch abandoned versions swept on open
}

TEST(SchemaStorageTest, schemaFooterAndInventoryAreVerified) {
  RAMDir dir;
  CommitSnapshotRegistry registry(test::reserveIncarnation(dir));
  IndexWriter writer(registry);
  auto snapshot = registry.snapshot();
  auto name = snapshot->schema->file.name;
  auto body = dir.openFile(name)->read();
  std::vector<std::byte> damaged((const std::byte*)body.data(), (const std::byte*)body.data() + body.size());
  damaged.back() ^= std::byte{1};
  dir.publishFile(name, damaged, Directory::PublishMode::REPLACE);
  SchemaCache cache;
  EXPECT_THROW(cache.load(dir, *readDurableIndexInfo(dir)), std::runtime_error);
  CommitSnapshotRegistry reopened(dir);
  EXPECT_THROW(reopened.openLocalSnapshot(true), std::exception);
}
