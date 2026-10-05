// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "luxir/server/Promotion.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class CollectionsTest : public LuxirTest {};

TEST_F(CollectionsTest, installActivatesOnlyOverTheExpectedEntry) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto& collections = node.collections();
  auto storage = collections.storage().collection("main");
  auto selected = *storage.current();
  auto copy = copySnapshot(storage, selected, Manifest::load(*storage.open(selected)));
  auto stage = [&] {
    auto candidate = collections.stage("main", copy);
    candidate.snapshots().openLocalSnapshot();
    return candidate;
  };

  // A changed map is detected before CURRENT moves.
  EXPECT_THROW(collections.install(stage(), nullptr), std::runtime_error);
  EXPECT_EQ(selected, storage.current());

  auto active = collections.get("main");
  auto installed = collections.install(stage(), active);
  EXPECT_EQ(installed, collections.get("main"));
  EXPECT_EQ(copy, storage.current());
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
  EXPECT_EQ(1, installed->getReaderManager().getReader()->liveDocs());

  // Abandoning a candidate never removes the selected incarnation.
  auto again = collections.stage("main", copy);
  collections.discard(again);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
}
