// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <span>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

// Field-retrieval micro-benchmarks over the shared bench index:
//
//   BM_FieldLoad   - a top-100 page retrieval with a fixed, cheap query.
//                    The "none" variant (no fields) is the control: subtract
//                    it to isolate field-loading cost.  ROWS vs COLUMNS
//                    measures the scatter pass on top of the same loaders.
//
//   BM_JsonRender  - renderSearchResponseBody over an already-built
//                    response (built once, outside the timing loop): the
//                    columns->rows transpose vs the rows passthrough.

static constexpr int32_t benchDocs = 10'000'000;
static constexpr const char* shape = "9555";  // 9 segments, 555 docs per segment

static const std::vector<std::string> FIELDS_NONE = {};
static const std::vector<std::string> FIELDS_INT = {"u10k_i"};
static const std::vector<std::string> FIELDS_STR = {"med_u10k_s"};
static const std::vector<std::string> FIELDS_MIXED = {"id", "u10k_i", "short_u10k_s", "med_u10k_s"};

// Build or reuse the standard bench index; returns whether it was reused.
static bool setupIndex(CollectionHelper& helper, int64_t& nDocs) {
  nDocs = luxir::unit_tests ? LuxirTest::scaleTestWork(200) : benchDocs;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);
  bool reuse = helper.indexMatchesShape(docsPerSeg);
  if (!reuse) buildBenchIndex(helper, nDocs, docsPerSeg);
  return reuse;
}

// Fixed page query.  At full scale short_u10k_s:"0" matches ~nDocs/10k docs
// (~1k), so collection stays cheap relative to loading; the tiny unit-test
// corpus needs the lower-cardinality field to match anything at all.
static const char* pageQueryField() {
  return luxir::unit_tests ? "short_u10_s" : "short_u10k_s";
}

// Order-sensitive value hash over both carriers of a DocList (columns and
// docs), for the per-iteration determinism assert.
static int64_t fingerprintDocList(const luxir::api::DocList& dl) {
  int64_t h = dl.row_count;
  for (const auto& [name, col] : dl.columns) {
    h = h * 31 + java_string_hashcode(name);
    if (const auto* c = std::get_if<luxir::api::ColStr>(&col.kind)) {
      for (size_t i = 0; i < c->v.size(); i++)
        if (c->v[i] != c->missing_val) h = h * 31 + java_string_hashcode(c->v[i]);
    } else if (const auto* c = std::get_if<luxir::api::ColInt>(&col.kind)) {
      for (size_t i = 0; i < c->v.size(); i++)
        if (c->v[i] != c->missing_val) h = h * 31 + c->v[i];
    }
  }
  for (const auto& row : dl.docs) {
    for (const auto& [name, val] : row.fields) {
      h = h * 31 + java_string_hashcode(name);
      if (const auto* s = std::get_if<std::string_view>(&val->kind)) {
        h = h * 31 + java_string_hashcode(*s);
      } else if (const auto* i = std::get_if<int64_t>(&val->kind)) {
        h = h * 31 + *i;
      }
    }
  }
  return h;
}

static void BM_FieldLoad(benchmark::State& state, const std::vector<std::string>& fields,
                         luxir::api::DocFormat format) {
  CollectionHelper helper;
  int64_t nDocs = 0;
  bool reuseIndex = setupIndex(helper, nDocs);

  int64_t fp = -1;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection("main");
    req->topDocs("q")
        .matchQuery(pageQueryField(), "0")
        .fields(std::span<const std::string>(fields))
        .documentFormat(format)
        .limit(100);
    req->execute(false);

    const auto* dl = req->docList("q");
    ASSERT_NE(dl, nullptr);
    int64_t ret = fingerprintDocList(*dl);
    benchmark::DoNotOptimize(ret);
    if (fp != -1) {
      ASSERT_EQ(fp, ret);
    }
    fp = ret;
  }

  state.counters["fp"] = fp;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

// Separate corpus: the standard root benchmark index stays reusable. Each
// root has eight small children, or one root has a 4096-row family. The root
// twins retain comparable payloads as multi-valued fields without objects.
enum class NestedSelection { WHOLE, FIELD, DEEP, OWNER, FLAT };

static void BM_NestedFieldLoad(benchmark::State& state, NestedSelection selection,
                                luxir::api::DocFormat format) {
  int children = (int)state.range(0);
  bool large = children > 8;
  int roots = large ? 1 : (luxir::unit_tests ? LuxirTest::scaleTestWork(100) : 1000);
  CollectionHelper helper("retrieval_nested");
  helper.clear();
  SchemaBuilder schema;
  auto& notes = schema.field("notes_w");
  notes.type = api::FieldDef::FieldClass::TEXT;
  notes.multi = api::FieldDef::MultiMode::ARRAY;
  notes.stored = true;
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  for (int root = 0; root < roots; root++) {
    std::string items, colors, texts, materials;
    for (int child = 0; child < children; child++) {
      auto color = "color" + std::to_string(child);
      auto material = "material" + std::to_string(child);
      std::string text = "a stored item description " + std::to_string(child);
      if (child) { items += ','; colors += ','; texts += ','; materials += ','; }
      items += "{\"color_s\":\"" + color + "\",\"text_w\":\"" + text
          + "\",\"parts_nested\":[{\"material_s\":\"" + material + "\"}]}";
      colors += "\"" + color + "\"";
      texts += "\"" + text + "\"";
      materials += "\"" + material + "\"";
    }
    std::string json = "{\"id\":\"" + std::to_string(root) + "\",\"items_nested\":[" + items
        + "],\"colors_ss\":[" + colors + "],\"notes_w\":[" + texts + "],\"materials_ss\":[" + materials + "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit();
  ASSERT_TRUE(helper.submit(input).success);
  std::vector<std::string> fields;
  switch (selection) {
    case NestedSelection::WHOLE: fields = {"items_nested"}; break;
    case NestedSelection::FIELD: fields = {"items_nested.color_s"}; break;
    case NestedSelection::DEEP: fields = {"items_nested.parts_nested.material_s"}; break;
    case NestedSelection::OWNER: fields = {"color_s", "/id"}; break;
    case NestedSelection::FLAT: fields = {"colors_ss", "notes_w", "materials_ss"}; break;
  }
  // One response batch holds at most 256 rows.
  int limit = large ? (selection == NestedSelection::OWNER ? 256 : 1) : 100;
  int64_t fp = -1;
  for (auto _ : state) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->collection("retrieval_nested").topDocs("q").allQuery()
        .fields(std::span<const std::string>(fields)).documentFormat(format).limit(limit).batchSize(limit);
    if (selection == NestedSelection::OWNER) op.rawOp().domain.emplace().to = "items_nested";
    req->execute(false);
    ASSERT_OK(req);
    const auto* docs = req->docList("q");
    ASSERT_NE(nullptr, docs);
    ASSERT_EQ(std::min(limit, selection == NestedSelection::OWNER ? roots * children : roots), docs->row_count);
    auto value = fingerprintDocList(*docs);
    benchmark::DoNotOptimize(docs);
    if (fp != -1) { ASSERT_EQ(fp, value); }
    fp = value;
  }
  state.counters["fp"] = fp;
  state.counters["children"] = children;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

// JSON render corpus: its own tiny collection (one full response batch) so
// the shared bench index is never touched.  One string column followed by
// alternating int/float/double columns makes per-cell column-type branches
// unpredictable - the case that punishes re-dispatching the column variant
// for every cell instead of once per column.
static const std::vector<std::string> FIELDS_MIXED7 = {"id", "a_i", "a_f", "a_d",
                                                       "b_i", "b_f", "b_d"};
// id-sized strings only: per-value fixed costs (key prefix, quoting, short
// escape scans) with almost no payload bytes.
static const std::vector<std::string> FIELDS_ID = {"id"};

// Render an already-built response repeatedly; the request handle stays in
// the caller's scope so the response arena outlives the loop.
// bytes_per_second reports render throughput.
static void renderLoop(benchmark::State& state, const luxir::api::SearchResponse& proto) {
  int64_t fp = -1;
  for (auto _ : state) {
    std::string line = renderSearchResponseBody(proto);
    int64_t ret = (int64_t)line.size();
    benchmark::DoNotOptimize(line.data());
    if (fp != -1) {
      ASSERT_EQ(fp, ret);
    }
    fp = ret;
  }
  state.counters["bytes"] = fp;
  state.SetBytesProcessed(state.iterations() * fp);
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

static void BM_JsonRender(benchmark::State& state, const std::vector<std::string>& fields,
                          luxir::api::DocFormat format) {
  CollectionHelper helper("jsonbm");
  helper.clear();  // idempotent across variants and repeated --bench runs
  std::vector<Doc> corpus;
  SplitMix64 r(42);
  for (int i = 0; i < 100; i++) {
    corpus.push_back(flatdoc("id", "doc" + std::to_string(i),
                             "a_i", r.rint(1'000'000),
                             "a_f", (float)r.rint(1'000'000) / 100.0f,
                             "a_d", (double)r.rint(1'000'000'000) / 1000.0,
                             "b_i", r.rint(1'000'000),
                             "b_f", (float)r.rint(1'000'000) / 100.0f,
                             "b_d", (double)r.rint(1'000'000'000) / 1000.0));
  }
  helper.indexAll(corpus, UpdateMessage::COMMIT);

  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  req->collection("jsonbm");
  req->topDocs("q")
      .allQuery()
      .fields(std::span<const std::string>(fields))
      .documentFormat(format)
      .limit(100);
  req->execute(false);
  ASSERT_NE(req->docList("q"), nullptr);
  ASSERT_EQ(100, req->docList("q")->row_count);
  renderLoop(state, req->responses[0]->proto);
}

// Really big strings: megabyte-scale stored text bodies, the case where the
// per-byte string-escape loop is the whole cost.  Mostly clean prose with
// newlines (escaped) and the occasional quote, so the escape path is
// exercised at a realistic density rather than being dead code.
static std::string makeBody(SplitMix64& r, size_t targetBytes) {
  static constexpr const char* words[] = {
      "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dogs",
      "search", "engine", "column", "stored", "field", "chunk", "postings"};
  std::string s;
  s.reserve(targetBytes + 16);
  int wordsInLine = 0;
  while (s.size() < targetBytes) {
    s += words[r.rint(std::size(words))];
    if (r.rint(97) == 0) s += '"';
    if (++wordsInLine >= 12) {
      s += '\n';
      wordsInLine = 0;
    } else {
      s += ' ';
    }
  }
  return s;
}

static void BM_JsonRenderBody(benchmark::State& state, luxir::api::DocFormat format) {
  size_t bodyBytes = luxir::unit_tests ? 32 * 1024 : 1024 * 1024;
  CollectionHelper helper("jsonbm_body");
  helper.clear();
  std::vector<Doc> corpus;
  SplitMix64 r(7);
  for (int i = 0; i < 10; i++) {
    corpus.push_back(flatdoc("id", "doc" + std::to_string(i),
                             "body_t", makeBody(r, bodyBytes)));
  }
  helper.indexAll(corpus, UpdateMessage::COMMIT);

  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  req->collection("jsonbm_body");
  req->topDocs("q")
      .allQuery()
      .fields({"id", "body_t"})
      .documentFormat(format)
      .limit(10);
  req->execute(false);
  ASSERT_NE(req->docList("q"), nullptr);
  ASSERT_EQ(10, req->docList("q")->row_count);
  renderLoop(state, req->responses[0]->proto);
}

static constexpr auto ROWS = luxir::api::DocFormat::ROWS;
static constexpr auto COLS = luxir::api::DocFormat::COLUMNS;

LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, none, FIELDS_NONE, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, int_cols, FIELDS_INT, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, int_rows, FIELDS_INT, ROWS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, str_cols, FIELDS_STR, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, str_rows, FIELDS_STR, ROWS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, mixed4_cols, FIELDS_MIXED, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_FieldLoad, mixed4_rows, FIELDS_MIXED, ROWS);

LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, whole_rows, NestedSelection::WHOLE, ROWS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, whole_cols, NestedSelection::WHOLE, COLS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, field_rows, NestedSelection::FIELD, ROWS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, field_cols, NestedSelection::FIELD, COLS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, deep_rows, NestedSelection::DEEP, ROWS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, deep_cols, NestedSelection::DEEP, COLS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, owner_rows, NestedSelection::OWNER, ROWS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, owner_cols, NestedSelection::OWNER, COLS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, flat_rows, NestedSelection::FLAT, ROWS)->Arg(8)->Arg(4096);
LUXIR_BENCHMARK_CAPTURE(BM_NestedFieldLoad, flat_cols, NestedSelection::FLAT, COLS)->Arg(8)->Arg(4096);

LUXIR_BENCHMARK_CAPTURE(BM_JsonRender, mixed7_cols, FIELDS_MIXED7, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_JsonRender, mixed7_rows, FIELDS_MIXED7, ROWS);
LUXIR_BENCHMARK_CAPTURE(BM_JsonRender, ids_cols, FIELDS_ID, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_JsonRender, ids_rows, FIELDS_ID, ROWS);
LUXIR_BENCHMARK_CAPTURE(BM_JsonRenderBody, body_cols, COLS);
LUXIR_BENCHMARK_CAPTURE(BM_JsonRenderBody, body_rows, ROWS);
