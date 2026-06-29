// Standalone validation of the concrete-API test infra: exercises CollectionHelper
// (Doc -> concrete solux::api::Map build-by-backing, blocking + async update) and LocalReq
// (fluent OpCursor builder -> non-owning response via Val accessors) end to end.

#include <memory_resource>

#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

class SmokeTest : public SoluxTest {};

// convertDocToProto covers every FieldVal arm; this builds a doc with all of them into an
// arena and checks each Val variant arm (including the vector arms the engine round-trip
// below does not configure a schema for).
TEST_F(SmokeTest, ConvertDocAllArmsRoundTrip) {
  Doc doc = flatdoc(
    "b", true,
    "i", (int64_t)42,
    "f", 1.5f,
    "d", 2.5,
    "s", std::string("hello"),
    "ai", std::vector<int64_t>{1, 2, 3},
    "af", std::vector<float>{1, 2, 3, 4},           // dense vector arm
    "ad", std::vector<double>{1.1, 2.2},
    "as", std::vector<std::string>{"x", "y"},
    "av", std::vector<std::vector<float>>{{1, 2}, {3, 4, 5}}  // multi-vector arm
  );

  std::pmr::monotonic_buffer_resource arena;
  solux::api::Map map;
  CollectionHelper::convertDocToProto(doc, map, arena);

  // map.fields is a flat span of {key, indirect_view<Val>} pairs (build order, no hashing),
  // so look fields up by linear scan.
  static const solux::api::Val kMissing;
  auto arm = [&](const char* name) -> const solux::api::Val& {
    for (const auto& kv : map.fields) {
      if (kv.first == name) return *kv.second;
    }
    ADD_FAILURE() << "missing field " << name;
    return kMissing;
  };

  EXPECT_EQ(std::get<bool>(arm("b").kind), true);
  EXPECT_EQ(std::get<int64_t>(arm("i").kind), 42);
  EXPECT_FLOAT_EQ(std::get<float>(arm("f").kind), 1.5f);
  EXPECT_DOUBLE_EQ(std::get<double>(arm("d").kind), 2.5);
  EXPECT_EQ(std::get<std::string_view>(arm("s").kind), "hello");

  const auto& ai = std::get<solux::api::ArrInt>(arm("ai").kind).v;
  ASSERT_EQ(ai.size(), 3u);
  EXPECT_EQ(ai[0], 1); EXPECT_EQ(ai[2], 3);

  const auto& af = std::get<solux::api::Vector>(arm("af").kind).f32;
  ASSERT_TRUE(af.has_value());
  ASSERT_EQ(af->v.size(), 4u);
  EXPECT_FLOAT_EQ(af->v[3], 4);

  const auto& ad = std::get<solux::api::ArrDouble>(arm("ad").kind).v;
  ASSERT_EQ(ad.size(), 2u);
  EXPECT_DOUBLE_EQ(ad[1], 2.2);

  const auto& as = std::get<solux::api::ArrStr>(arm("as").kind).v;
  ASSERT_EQ(as.size(), 2u);
  EXPECT_EQ(as[0], "x"); EXPECT_EQ(as[1], "y");

  const auto& av = std::get<solux::api::ArrVector>(arm("av").kind).v;
  ASSERT_EQ(av.size(), 2u);
  ASSERT_TRUE(av[0].f32.has_value());
  EXPECT_EQ(av[0].f32->v.size(), 2u);
  ASSERT_TRUE(av[1].f32.has_value());
  EXPECT_EQ(av[1].f32->v.size(), 3u);
}

// Full data path: index (sync) -> commit -> search via fluent builder -> non-owning response.
TEST_F(SmokeTest, IndexSearchRoundTrip) {
  CollectionHelper helper;
  helper.clear();

  helper.index(flatdoc("id", std::string("1"), "foo_w", "how now brown cow",
                       "foo_i", (int64_t)17, "color_s", "red", "colors_ss", vecs("red", "green")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", std::string("2"), "foo_w", "charlie brown",
                       "foo_i", (int64_t)23, "color_s", "blue"),
               UpdateMessage::NO_COMMIT);
  std::vector<Doc> more{flatdoc("id", std::string("3"), "foo_w", "brown",
                               "foo_i", (int64_t)5, "color_s", "brown",
                               "colors_ss", vecs("red", "black"))};
  helper.indexAll(more, UpdateMessage::COMMIT);

  // "brown" appears in all three docs.
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    req->topDocs("q").matchQuery("foo_w", "brown")
        .fields({"id", "color_s", "colors_ss", "foo_i"})
        .withStats()
        .limit(10);
    req->execute();
    ASSERT_OK(req);
    EXPECT_EQ(req->getMatchCount("q"), 3);
    auto docs = req->getDocs("q");
    EXPECT_EQ(docs.size(), 3u);

    // Locate doc id "3" and verify its scalar + multi-valued columns survived.
    const Doc* d3 = nullptr;
    for (const auto& d : docs) {
      if (const auto* idv = find(d, "id"); idv && std::get<std::string>(*idv) == "3") d3 = &d;
    }
    ASSERT_NE(d3, nullptr) << req->toString();
    ASSERT_NE(find(*d3, "foo_i"), nullptr);
    EXPECT_EQ(std::get<int64_t>(*find(*d3, "foo_i")), 5);
    EXPECT_EQ(std::get<std::string>(*find(*d3, "color_s")), "brown");
    ASSERT_NE(find(*d3, "colors_ss"), nullptr);
    // Multi-valued string columns return sorted+deduped (SortedSet semantics), so the
    // stored {"red","black"} comes back as {"black","red"}.
    EXPECT_EQ(std::get<std::vector<std::string>>(*find(*d3, "colors_ss")),
              (std::vector<std::string>{"black", "red"}));
  }

  // A more selective query.
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    req->topDocs("q").matchQuery("foo_w", "charlie").withStats().limit(10);
    req->execute();
    ASSERT_OK(req);
    EXPECT_EQ(req->getMatchCount("q"), 1);
  }

  // allQuery returns everything.
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    req->topDocs("q").allQuery().withStats().limit(10);
    req->execute();
    ASSERT_OK(req);
    EXPECT_EQ(req->getMatchCount("q"), 3);
  }
}

// Async index path: the update message must own its build arena + request view until done()
// fires the callback.
TEST_F(SmokeTest, AsyncIndex) {
  CollectionHelper helper;
  helper.clear();

  Blocker blocker;
  bool ok = false;
  helper.index(flatdoc("id", std::string("async1"), "foo_w", "async brown fox", "foo_i", (int64_t)99),
               [&](const IndexResult& result) {
                 ok = result.success;
                 blocker.notify();
               },
               UpdateMessage::COMMIT);
  blocker.wait();
  EXPECT_TRUE(ok);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").matchQuery("foo_w", "fox").withStats().limit(10);
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(req->getMatchCount("q"), 1);
}
