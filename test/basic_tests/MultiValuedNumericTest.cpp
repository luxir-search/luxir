// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// Multi-valued numeric columns hold each doc's values ascending (numeric
// order, duplicates kept), through flush and merge.
class MultiValuedNumericTest : public LuxirTest {
protected:
  void indexDocs(CollectionHelper& helper) {
    helper.index(flatdoc("id_s", "a", "n_is", vec_i(9, -4, 9, 0), "d_ds", vec(2.5, -7.0, -0.5)),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "b", "n_is", vec_i(8, 7), "d_ds", vec(1e300, -1e-300)),
                 UpdateMessage::COMMIT);
    helper.index(flatdoc("id_s", "c", "n_is", vec_i(20, 1), "d_ds", vec(3.0)),
                 UpdateMessage::COMMIT);
  }

  // Doc ids and each doc's emitted values, sorted by `sortExpr`.
  struct Rows {
    std::vector<std::string> ids;
    std::vector<std::vector<int64_t>> ints;
    std::vector<std::vector<double>> doubles;
  };

  Rows run(std::string_view sortExpr, qb::SortDir dir) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s", "n_is", "d_ds"});
    qb::sort(top, sortExpr, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto& docs = *req->docList("q");
    Rows rows;
    for (auto id : std::get<api::ColStr>(docs.columns.at("id_s").kind).v) {
      rows.ids.emplace_back(id);
    }
    for (const auto& arr : std::get<api::ArrArrInt>(docs.columns.at("n_is").kind).v) {
      rows.ints.emplace_back(arr.v.begin(), arr.v.end());
    }
    for (const auto& arr : std::get<api::ArrArrDouble>(docs.columns.at("d_ds").kind).v) {
      rows.doubles.emplace_back(arr.v.begin(), arr.v.end());
    }
    return rows;
  }

  void expectSortedAndOrdered() {
    auto rows = run("id_s", qb::ASC);
    ASSERT_EQ((std::vector<std::string>{"a", "b", "c"}), rows.ids);
    EXPECT_EQ((std::vector<int64_t>{-4, 0, 9, 9}), rows.ints[0]);
    EXPECT_EQ((std::vector<int64_t>{7, 8}), rows.ints[1]);
    EXPECT_EQ((std::vector<int64_t>{1, 20}), rows.ints[2]);
    EXPECT_EQ((std::vector<double>{-7.0, -0.5, 2.5}), rows.doubles[0]);
    EXPECT_EQ((std::vector<double>{-1e-300, 1e300}), rows.doubles[1]);

    // min ascending, max descending: ints a=-4/9, b=7/8, c=1/20.
    EXPECT_EQ((std::vector<std::string>{"a", "c", "b"}), run("n_is", qb::ASC).ids);
    EXPECT_EQ((std::vector<std::string>{"c", "a", "b"}), run("n_is", qb::DESC).ids);
    // doubles a=-7/2.5, b=-1e-300/1e300, c=3/3.
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), run("d_ds", qb::ASC).ids);
    EXPECT_EQ((std::vector<std::string>{"b", "c", "a"}), run("d_ds", qb::DESC).ids);
    EXPECT_EQ((std::vector<std::string>{"b", "c", "a"}), run("max(d_ds)", qb::DESC).ids);
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), run("min(d_ds)", qb::ASC).ids);
  }
};

TEST_F(MultiValuedNumericTest, valuesSortedPerDocAndSortReadsEdges) {
  CollectionHelper helper;
  indexDocs(helper);
  expectSortedAndOrdered();
}

TEST_F(MultiValuedNumericTest, mergePreservesPerDocOrder) {
  CollectionHelper helper;
  indexDocs(helper);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  expectSortedAndOrdered();
}
