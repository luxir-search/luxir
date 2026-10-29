// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/store/ObjectStore.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

// Detaching a collection leaves its storage for another node; attaching opens
// one that storage lists and the node has not opened.

using namespace luxir;
using namespace luxir::test;

namespace {

class CollectionDetachTest : public ::testing::Test {
protected:
  std::string name = "detach_" + newUuid();
  LuxirConfig config;
  CollectionId id = CollectionId::of("main");

  void SetUp() override {
    config.store.backend = "ram_object";
    config.store.options = {{"store_name", name}};
  }
  void TearDown() override { RamObjectStore::drop(name); }

  static uint64_t liveDocs(LuxirNode& node) {
    return node.getCollection("main")->getReaderManager().getReader()->liveDocs();
  }
};

}  // namespace

// Committed and buffered documents move with the collection: here, and to the
// next node over the store.
TEST_F(CollectionDetachTest, aDetachedCollectionMovesWithEveryDocument) {
  {
    LuxirNode node(config);
    {
      CollectionHelper h(node, "main");
      ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
      ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
      ASSERT_TRUE(h.indexAll({flatdoc("id", "c")}, UpdateMessage::NO_COMMIT).success);
    }
    node.detachCollection(id);
    EXPECT_THROW(node.getCollection(id), CollectionNotFoundError);
    EXPECT_THROW(node.detachCollection(id), CollectionNotFoundError);
    EXPECT_EQ(1u, node.collections().storage().collections().size()) << "storage keeps it";
    node.attachCollection(id);
    EXPECT_EQ(2u, liveDocs(node));
    EXPECT_THROW(node.attachCollection(id), CollectionExistsError);
    EXPECT_THROW(node.attachCollection(CollectionId::of("nowhere")), CollectionNotFoundError);
    node.detachCollection(id);
  }
  LuxirNode next(config);
  EXPECT_EQ(2u, liveDocs(next));
}

TEST_F(CollectionDetachTest, onlyAWriterDetachesOrAttaches) {
  {
    LuxirNode writer(config);
    CollectionHelper h(writer, "main");
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  }
  auto readOnly = config;
  readOnly.read_only = true;
  LuxirNode reader(readOnly);
  EXPECT_THROW(reader.detachCollection(id), ReadOnlyError);
  EXPECT_THROW(reader.attachCollection(id), ReadOnlyError);
}
