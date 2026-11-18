// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include "luxir/index/SegmentMerger.h"
#include <map>
#include <set>
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

namespace luxir::test {
namespace {

struct ItemModel {
  int64_t tag = -1;
  std::vector<std::string> colors;
  std::string size, text;
  std::vector<std::string> parts;
  int32_t price = 0;
};
struct RootModel {
  std::string id;
  bool live = true, eligible = false, present = false;
  std::vector<ItemModel> items;
};
struct JoinCase {
  int kind;
  std::string color, size, material;
  int32_t low, high;
};
using Ids = std::set<std::string>;

class NestedJoinRandomTest : public LuxirTest {
protected:
  CollectionHelper helper;
  static constexpr int KINDS = 13;
  static constexpr uint64_t SEED = 0x9e3779b97f4a7c15ULL;
  static constexpr std::string_view RANKED = "body_w:(alpha OR beta^0.001 OR gamma^0.001)";
  uint64_t state = SEED;
  int64_t nextTag = 0;
  std::vector<RootModel> model;
  const std::array<std::string, 4> colors{"red", "blue", "green", "yellow"};
  const std::array<std::string, 3> sizes{"S", "M", "L"}, materials{"steel", "wood", "glass"};

  uint64_t random() {
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dULL;
  }

  RootModel generate(int32_t id) {
    RootModel root{"r" + std::to_string(id), true, random() % 2 != 0, random() % 2 != 0, {}};
    int32_t count = id == 0 ? 640 : id == 31 ? 40 : (int32_t)(random() % 6);
    for (int32_t i = 0; i < count; i++) {
      ItemModel item;
      if (random() % 11 != 0) {
        item.tag = nextTag++;
        for (size_t n = random() % 4; n; --n) item.colors.push_back(colors[random() % colors.size()]);
        if (random() % 4) item.size = sizes[random() % sizes.size()];
        item.price = random() % 4 ? 1 + (int32_t)(random() % 1000) : 0;
        const char* texts[]{"alpha beta", "alpha x beta", "alpha", "beta", "rare alpha beta", ""};
        item.text = texts[random() % std::size(texts)];
        for (size_t n = random() % 4; n; --n) {
          item.parts.push_back(random() % 5 ? materials[random() % materials.size()] : "");
        }
      }
      root.items.push_back(std::move(item));
    }
    return root;
  }

  static std::string json(const RootModel& root) {
    std::string out = "{\"id\":\"" + root.id + "\",\"owner_i\":" + (root.eligible ? "1" : "0");
    out += ",\"body_w\":\"gamma";
    int32_t ordinal = std::stoi(root.id.substr(1));
    for (int32_t i = 0; i < 64; i++) out += i <= ordinal ? " alpha" : " beta";
    out += '"';
    if (root.present || !root.items.empty()) {
      out += ",\"items_nested\":[";
      for (size_t i = 0; i < root.items.size(); i++) {
        if (i) out += ',';
        const auto& item = root.items[i];
        if (item.tag < 0 && i % 2 == 0) { out += "null"; continue; }
        out += '{';
        if (item.tag >= 0) {
          out += "\"tag_i\":" + std::to_string(item.tag);
          if (!item.colors.empty()) {
            out += ",\"color_ss\":[";
            for (size_t c = 0; c < item.colors.size(); c++) out += (c ? ",\"" : "\"") + item.colors[c] + '"';
            out += ']';
          }
          if (!item.size.empty()) out += ",\"size_s\":\"" + item.size + '"';
          if (!item.text.empty()) out += ",\"text_w\":\"" + item.text + '"';
          if (item.price) out += ",\"price_i\":" + std::to_string(item.price);
          out += ",\"parts_nested\":[";
          for (size_t p = 0; p < item.parts.size(); p++) {
            if (p) out += ',';
            out += item.parts[p].empty() ? (p % 2 ? "{}" : "null") : "{\"material_s\":\"" + item.parts[p] + "\"}";
          }
          out += ']';
        }
        out += '}';
      }
      out += ']';
    }
    return out + '}';
  }

  void add(CollectionHelper::UpdateBuilder& input, const RootModel& root) {
    auto value = json(root);
    input.addRaw([&](auto& doc, auto& arena) { EXPECT_TRUE(api::read_json(doc, value, arena)); });
  }

  static api::Query expr(std::string_view text) {
    api::Query query; query.kind.emplace<api::ExprQuery>().q = text; return query;
  }

  static bool itemMatches(const ItemModel& item, const JoinCase& q) {
    bool color = std::ranges::find(item.colors, q.color) != item.colors.end();
    bool size = item.size == q.size;
    bool material = std::ranges::find(item.parts, q.material) != item.parts.end();
    switch (q.kind) {
      case 0: case 11: return color;
      case 1: case 3: return color && size;
      case 2: return color && !size;
      case 4: return material;
      case 5: return color && material;
      case 6: return true;
      case 7: return !item.parts.empty();
      case 8: return item.text.find("alpha beta") != std::string::npos;
      case 9: return item.price >= q.low && item.price <= q.high;
      case 10: return !color;
      case 12: return color || size;
    }
    return false;
  }

  static bool matches(const RootModel& root, const JoinCase& q) {
    bool match = std::ranges::any_of(root.items, [&](const auto& item) { return itemMatches(item, q); });
    bool size = std::ranges::any_of(root.items, [&](const auto& item) { return item.size == q.size; });
    if (q.kind == 0) match &= size;
    if (q.kind == 11) match |= size;
    return q.kind == 3 ? !match : match;
  }

  static std::string inner(const JoinCase& q) {
    auto color = "color_ss:" + q.color;
    auto size = "size_s:" + q.size;
    switch (q.kind) {
      case 0: case 11: return color;
      case 1: case 3: return color + " AND " + size;
      case 2: return color + " AND NOT " + size;
      case 4: return "material_s:" + q.material;
      case 5: return color + " AND parts_nested:(material_s:" + q.material + ')';
      case 6: case 7: return "*:*";
      case 8: return "text_w:\"alpha beta\"";
      case 9: return "price_i:[" + std::to_string(q.low) + " TO " + std::to_string(q.high) + ']';
      case 10: return "NOT " + color;
      case 12: return color + " OR " + size;
    }
    return {};
  }

  static std::string expression(const JoinCase& q, bool nested = false) {
    if (q.kind == 0 || q.kind == 11) return "items_nested.color_ss:" + q.color
        + (q.kind == 0 ? " AND " : " OR ") + "items_nested.size_s:" + q.size;
    if (q.kind == 4) return nested ? "items_nested:(parts_nested:(" + inner(q) + "))"
        : "items_nested.parts_nested.material_s:" + q.material;
    if (q.kind == 6) return "items_nested:*";
    if (q.kind == 7) return "items_nested.parts_nested:*";
    return (q.kind == 3 ? "NOT " : "") + std::string("items_nested:(") + inner(q) + ')';
  }

  api::Query explicitQuery(LocalReq& req, const JoinCase& q, const char* score) {
    auto path = q.kind == 4 || q.kind == 7 ? "items_nested.parts_nested" : "items_nested";
    auto body = api::build::arenaStr(req.mr, inner(q));
    auto join = qb::toParent(req.mr, path, expr(body), score);
    if (q.kind == 0 || q.kind == 11) {
      auto other = qb::toParent(req.mr, path, qb::match(req.mr, "size_s", q.size), score);
      return q.kind == 0 ? qb::boolean(req.mr, {join, other}) : qb::boolean(req.mr, {}, {join, other});
    }
    if (q.kind == 3) return qb::boolean(req.mr, {qb::all()}, {}, {join});
    return join;
  }

  void checkQueries(const std::vector<JoinCase>& queries, bool cached) {
    auto body = localReq(helper.getSearchEngine());
    body->topDocs().exprQuery(RANKED).getScores()
        .countLimit().limit(-1).fields({"id"});
    body->execute(1); ASSERT_OK(body);
    auto bodyDocs = body->getDocs();
    auto bodyScores = std::get<api::ColFloat>(body->docList()->columns.find("_score_")->kind).v;
    for (size_t i = 0; i < queries.size(); i++) {
      const auto& q = queries[i]; int32_t position = (int32_t)(i / KINDS);
      Ids expected;
      for (const auto& root : model) {
        if (!root.live) continue;
        bool yes = matches(root, q);
        if (position == 2) yes = !yes;
        if (position == 3) yes &= root.eligible;
        if (yes) expected.insert(root.id);
      }
      int forms = q.kind == 4 ? 4 : 3;
      for (int form = 0; form < forms; form++) for (int repeat = 0; repeat < (cached ? 3 : 2); repeat++) {
        SCOPED_TRACE("query=" + std::to_string(i) + " " + expression(q) + " form=" + std::to_string(form)
            + " cached=" + std::to_string(cached) + " repeat=" + std::to_string(repeat));
        auto req = localReq(helper.getSearchEngine());
        auto query = form == 0 || form == 3
            ? expr(api::build::arenaStr(req->mr, expression(q, form == 3)))
            : explicitQuery(*req, q, form == 1 ? "max" : "none");
        auto& op = req->topDocs().countLimit().limit(-1).batchSize(4096).fields({"id"});
        if (position == 0) op.rawQuery() = query;
        if (position == 1) op.allQuery().filter(query);
        if (position == 2) op.rawQuery() = qb::boolean(req->mr, {qb::all()}, {}, {query});
        if (position == 3) op.rawQuery() = qb::boolean(req->mr, {qb::match(req->mr, "owner_i", "1"), query});
        if (position == 1 && q.kind == 1) {
          req->topDocs("ranked").exprQuery(RANKED)
              .getScores().limit(5).fields({"id"}).filter(query);
        }
        req->execute(1); ASSERT_OK(req);
        Ids actual;
        for (const auto& doc : req->getDocs()) actual.insert(std::get<std::string>(*find(doc, "id")));
        EXPECT_EQ(expected, actual); EXPECT_EQ((int64_t)expected.size(), req->getMatchCount());
        if (position == 1 && q.kind == 1) {
          auto ranked = req->getDocs("ranked");
          std::vector<size_t> selected;
          for (size_t n = 0; n < bodyDocs.size() && selected.size() < 5; n++) {
            if (expected.contains(std::get<std::string>(*find(bodyDocs[n], "id")))) selected.push_back(n);
          }
          ASSERT_EQ(selected.size(), ranked.size());
          if (!ranked.empty()) {
            auto scores = std::get<api::ColFloat>(req->docList("ranked")->columns.find("_score_")->kind).v;
            for (size_t n = 0; n < ranked.size(); n++) {
              EXPECT_EQ(std::get<std::string>(*find(bodyDocs[selected[n]], "id")),
                  std::get<std::string>(*find(ranked[n], "id")));
              EXPECT_FLOAT_EQ(bodyScores[selected[n]], scores[n]);
            }
          }
        }
      }
    }
  }

  void checkDomains() {
    int64_t rows = 0, owners = 0, parts = 0, itemOwners = 0, rootOwners = 0;
    for (const auto& root : model) if (root.live && root.eligible) {
      rows += root.items.size(); owners += !root.items.empty(); bool hasParts = false;
      for (const auto& item : root.items) {
        parts += item.parts.size(); itemOwners += !item.parts.empty(); hasParts |= !item.parts.empty();
      }
      rootOwners += hasParts;
    }
    auto req = localReq(helper.getSearchEngine());
    auto& root = req->topDocs().matchQuery("owner_i", "1").countLimit().limit(0);
    auto& items = root.topDocs("items").allQuery().countLimit().limit(0);
    items.rawOp().domain.emplace().to = "items_nested";
    items.topDocs("owners").allQuery().countLimit().limit(0).rawOp().domain.emplace().to = "..";
    auto& deep = items.topDocs("parts").allQuery().countLimit().limit(0);
    deep.rawOp().domain.emplace().to = "parts_nested";
    deep.topDocs("items").allQuery().countLimit().limit(0).rawOp().domain.emplace().to = "..";
    deep.topDocs("roots").allQuery().countLimit().limit(0).rawOp().domain.emplace().to = "../..";
    deep.topDocs("absolute").allQuery().countLimit().limit(0).rawOp().domain.emplace().to = "/";
    req->execute(1); ASSERT_OK(req);
    auto* children = (*req->docList()->ops.find("items"))->docList();
    ASSERT_NE(nullptr, children); EXPECT_EQ(rows, *children->found);
    EXPECT_EQ(owners, *(*children->ops.find("owners"))->docList()->found);
    auto* grandchildren = (*children->ops.find("parts"))->docList();
    ASSERT_NE(nullptr, grandchildren); EXPECT_EQ(parts, *grandchildren->found);
    EXPECT_EQ(itemOwners, *(*grandchildren->ops.find("items"))->docList()->found);
    EXPECT_EQ(rootOwners, *(*grandchildren->ops.find("roots"))->docList()->found);
    EXPECT_EQ(rootOwners, *(*grandchildren->ops.find("absolute"))->docList()->found);
  }

  void checkReductions(const std::map<int64_t, float>& values,
                       const std::map<int64_t, std::string>& tags, std::string_view text) {
    auto order = localReq(helper.getSearchEngine());
    order->topDocs().allQuery().fields({"id"}).limit(-1);
    order->execute(1); ASSERT_OK(order);
    std::map<std::string, size_t> ordinal;
    for (const auto& doc : order->getDocs()) ordinal.emplace(std::get<std::string>(*find(doc, "id")), ordinal.size());
    std::map<std::string, std::vector<float>> rows;
    for (auto [tag, value] : values) rows[tags.at(tag)].push_back(value);
    for (auto mode : {"max", "min", "sum", "avg", "none"}) {
      int32_t minimum = 1 + (int32_t)(random() % 4);
      int32_t maximum = random() % 2 ? minimum + (int32_t)(random() % 4) : INT32_MAX;
      std::vector<std::pair<std::string, float>> expected;
      for (const auto& [id, scores] : rows) {
        if ((int64_t)scores.size() < minimum || (int64_t)scores.size() > maximum) continue;
        float value = mode == std::string_view("min") ? *std::ranges::min_element(scores) : *std::ranges::max_element(scores);
        if (mode == std::string_view("sum") || mode == std::string_view("avg")) {
          double sum = 0; for (float score : scores) sum += score;
          value = (float)(mode == std::string_view("avg") ? sum / (double)scores.size() : sum);
        } else if (mode == std::string_view("none")) value = 0;
        expected.emplace_back(id, value);
      }
      std::ranges::sort(expected, [&](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : ordinal.at(a.first) < ordinal.at(b.first);
      });
      for (int64_t count : {(int64_t)0, (int64_t)1, (int64_t)expected.size(), (int64_t)expected.size()+1, (int64_t)-1}) {
        for (int repeat = 0; repeat < 3; repeat++) {
          SCOPED_TRACE(::testing::Message() << mode << '/' << minimum << '/' << maximum << '/' << count << '/' << repeat);
          auto req = localReq(helper.getSearchEngine());
          auto query = qb::toParent(req->mr, "items_nested", expr(text), mode);
          auto& join = std::get<api::ToParentQuery>(query.kind);
          join.min_children = minimum; join.max_children = maximum;
          req->topDocs().getScores().countLimit(count).limit(repeat ? 100 : 4).fields({"id"}).rawQuery() = query;
          req->execute(1); ASSERT_OK(req);
          auto docs = req->getDocs();
          ASSERT_EQ(std::min(expected.size(), repeat ? (size_t)100 : (size_t)4), docs.size());
          if (!docs.empty()) {
            auto scores = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
            for (size_t i = 0; i < docs.size(); i++) {
              EXPECT_EQ(expected[i].first, std::get<std::string>(*find(docs[i], "id")));
              EXPECT_EQ(expected[i].second, scores[i]);
            }
          }
          if (count) {
            if (req->docList()->found_gte) { EXPECT_EQ(count, *req->docList()->found); EXPECT_GE(expected.size(), (size_t)count); }
            else { EXPECT_EQ(expected.size(), *req->docList()->found); }
          } else { EXPECT_FALSE(req->docList()->found.has_value()); }
        }
      }
    }
  }

  void checkScores(bool phrase) {
    std::map<int64_t, std::string> tags;
    for (const auto& root : model) if (root.live) for (const auto& item : root.items) {
      bool match = phrase ? item.text.find("alpha beta") != std::string::npos
          : item.text.find("alpha") != std::string::npos;
      if (match) tags.emplace(item.tag, root.id);
    }
    auto child = localReq(helper.getSearchEngine());
    std::string text = phrase ? "text_w:\"alpha beta\"" : "text_w:alpha";
    child->topDocs().exprQuery(text).getScores().countLimit().limit(-1).batchSize(4096).fields({"tag_i"})
        .rawOp().domain.emplace().to = "items_nested";
    child->execute(1); ASSERT_OK(child); EXPECT_EQ((int64_t)tags.size(), child->getMatchCount());
    std::map<std::string, float> best;
    std::map<int64_t, float> orderedScores;
    size_t seen = 0;
    for (const auto* response : child->responses) {
      auto* value = response->proto.ops.find("q");
      if (!value || !(**value).docList()) continue;
      const auto& columns = (**value).docList()->columns;
      auto* tagColumn = columns.find("tag_i");
      if (!tagColumn) continue;
      auto ids = std::get<api::ColInt>(tagColumn->kind).v;
      auto scores = std::get<api::ColFloat>(columns.find("_score_")->kind).v;
      ASSERT_EQ(ids.size(), scores.size());
      for (size_t i = 0; i < ids.size(); i++) {
        orderedScores.emplace(ids[i], scores[i]);
        ASSERT_TRUE(tags.contains(ids[i])); best[tags.at(ids[i])] = std::max(best[tags.at(ids[i])], scores[i]);
      }
      seen += ids.size();
    }
    EXPECT_EQ(tags.size(), seen);
    if (!phrase) checkReductions(orderedScores, tags, text);
    for (int form = 0; form < 3; form++) {
      auto req = localReq(helper.getSearchEngine());
      auto& op = req->topDocs().getScores().countLimit().limit(-1).batchSize(4096).fields({"id"});
      if (form == 0) op.rawQuery() = qb::toParent(req->mr, "items_nested", expr(text));
      else op.exprQuery(form == 1 ? "items_nested." + text : "items_nested:(" + text + ')');
      req->execute(1); ASSERT_OK(req); EXPECT_EQ((int64_t)best.size(), req->getMatchCount());
      auto actual = req->getDocs();
      auto values = std::get<api::ColFloat>(req->docList()->columns.find("_score_")->kind).v;
      ASSERT_EQ(best.size(), actual.size());
      for (size_t i = 0; i < actual.size(); i++) {
        auto id = std::get<std::string>(*find(actual[i], "id")); ASSERT_TRUE(best.contains(id));
        EXPECT_NEAR(best.at(id), values[i], 1e-5f * std::max(1.0f, best.at(id)));
      }
    }
  }
};

TEST_F(NestedJoinRandomTest, doubleExclusionOfJoinKeepsAnExactScorerProtocol) {
  CollectionHelper::UpdateBuilder input;
  for (auto json : {
      R"({"id":"a","items_nested":[{"color_ss":["red"],"size_s":"S"},{"color_ss":["blue"],"size_s":"L"}]})",
      R"({"id":"b","items_nested":{"color_ss":["red"],"size_s":"L"}})", R"({"id":"c"})"}) {
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  auto join = qb::toParent(req->mr, "items_nested", qb::boolean(req->mr,
      {qb::match(req->mr, "color_ss", "red"), qb::match(req->mr, "size_s", "L")}));
  auto excluded = qb::boolean(req->mr, {qb::all()}, {}, {join});
  req->topDocs().countLimit().fields({"id"}).rawQuery() = qb::boolean(req->mr, {qb::all()}, {}, {excluded});
  auto either = qb::boolean(req->mr, {}, {join,
      qb::toParent(req->mr, "items_nested", qb::match(req->mr, "color_ss", "blue"))});
  req->topDocs("union").countLimit().fields({"id"}).rawQuery() = qb::boolean(req->mr, {qb::all()}, {}, {either});
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(1, req->getMatchCount());
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", "b")));
  EXPECT_EQ(1, req->getMatchCount("union"));
  EXPECT_TRUE(containsDoc(req->getDocs("union"), flatdoc("id", "c")));
}

TEST_F(NestedJoinRandomTest, seededModelAcrossUpdatesDomainsScoresAndCache) {
  size_t peakMaps = 0;
  Signal::listen("mergeRowMaps", [&](void* value, void*, void*) -> void* {
    peakMaps = std::max(peakMaps, ((SegmentMerger*)value)->rowMapBytes());
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("mergeRowMaps"); });
  for (uint64_t seed : {SEED, (uint64_t)42, (uint64_t)0xdeadbeef}) {
    SCOPED_TRACE("seed=" + std::to_string(seed));
    std::cerr << "NestedJoinRandomTest seed=" << seed << '\n';
    helper.clear(); model.clear(); nextTag = 0; state = seed;
    helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    auto setting = [&](bool reverse) {
      SchemaBuilder schema;
      auto& outer = schema.templ("_nested"); outer.type = api::FieldDef::FieldClass::NESTED; outer.reverse_map = reverse;
      auto& inner = schema.field("items_nested.parts_nested"); inner.type = api::FieldDef::FieldClass::NESTED; inner.reverse_map = !reverse;
      schema.set(helper.collection());
    };
    auto cache = std::make_shared<FilterCache>(FilterCacheConfig{
        .maxBytes = 4 * 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
    helper.getIndexWriter()->snapshots.readers.filterCache = cache;
    for (int32_t id = 0; id < 64; id++) model.push_back(generate(id));
    model[1].items = {{nextTag++, {"red"}, "S", "alpha", {"steel"}, 100},
                      {nextTag++, {"blue"}, "L", "beta", {"wood"}, 600}};
    model[2].items = {ItemModel{}}; model[4].items.clear(); model[4].present = false;
    model[5].items.clear(); model[5].present = true;
    for (int batch = 0; batch < 3; batch++) {
      setting(batch % 2 != 0);
      CollectionHelper::UpdateBuilder input;
      for (int id = batch * 64 / 3; id < (batch + 1) * 64 / 3; id++) add(input, model[id]);
      input.overwrite().commit(); ASSERT_TRUE(helper.submit(input).success);
    }
    ASSERT_EQ(3u, helper.durableSegmentCount());
    std::vector<JoinCase> cases;
    for (int i = 0; i < 4 * KINDS; i++) cases.push_back({i % KINDS, colors[random() % colors.size()],
        sizes[random() % sizes.size()], materials[random() % materials.size()], 100, 500});
    cases[0].color = "red"; cases[0].size = "L";
    EXPECT_TRUE(matches(model[1], cases[0])); auto same = cases[0]; same.kind = 1; EXPECT_FALSE(matches(model[1], same));
    for (int phase = 0; phase < 4; phase++) {
      SCOPED_TRACE("phase=" + std::to_string(phase));
      // Leave admitted raw memberships resident across the root mutations.
      for (bool enabled : phase % 2 == 0 ? std::array{false, true} : std::array{true, false}) {
        cache->resize(enabled ? 4 * 1024 * 1024 : 0);
        auto hits = cache->counters().hits;
        checkQueries(cases, enabled); checkDomains(); checkScores(false); checkScores(true);
        if (enabled) { EXPECT_GT(cache->counters().hits, hits); EXPECT_GT(cache->counters().admissions, 0u); }
      }
      if (phase == 0 || phase == 2) {
        CollectionHelper::UpdateBuilder merge;
        merge.commit(true, phase == 0 ? 2 : 1);
        ASSERT_TRUE(helper.submit(merge).success);
        ASSERT_EQ(phase == 0 ? 2u : 1u, helper.durableSegmentCount());
      } else if (phase == 1) {
        setting(true);
        CollectionHelper::UpdateBuilder change;
        for (int id : {0, 7, 8}) { change.remove(model[id].id); model[id].live = false; }
        for (int id : {2, 11, 31, 11}) { model[id] = generate(id); add(change, model[id]); }
        change.overwrite().commit(); ASSERT_TRUE(helper.submit(change).success);
      }

    }
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(1u, reader->segments().size());
    std::array<int32_t, 3> rows{};
    for (const auto& root : model) if (root.live) {
      rows[0]++; rows[1] += (int32_t)root.items.size();
      for (const auto& item : root.items) rows[2] += (int32_t)item.parts.size();
    }
    const char* paths[]{"", "items_nested", "items_nested.parts_nested"};
    for (size_t i = 0; i < rows.size(); i++) {
      auto* space = reader->segments()[0].segmentCore().spaceReader(paths[i]);
      ASSERT_NE(nullptr, space); EXPECT_EQ(rows[i], space->maxDoc());
    }
    EXPECT_EQ(reader->maxDoc(), reader->liveDocs());
  }
  std::cerr << "NestedJoinRandomTest peak map bytes=" << peakMaps << '\n';
  EXPECT_GT(peakMaps, 0u);
}

} // namespace
} // namespace luxir::test
