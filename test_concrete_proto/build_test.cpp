// Build-by-backing proof: assemble a representative NON-OWNING SearchResponse with the
// solux::api::build helpers (the response-assembly primitives for the engine cutover),
// then round-trip BINARY (encode -> decode into a fresh arena) and JSON (write_json ->
// read_json). Exercises: opsSlot (multi-entry + nested), columnSlot, allocArray + index
// fill, arenaStr/arenaBytes, SpanBuilder (multi-valued, builder-dies-before-serialize),
// and Val oneof arms. Proves the build API end to end without the engine.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "solux_types.hpp"
#include "build.h"

using namespace std::string_view_literals;
namespace P = solux::api;
namespace B = solux::api::build;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("  FAIL: %s\n", #c); } } while (0)

// Build the same representative response into `mr`.
static P::SearchResponse buildResponse(std::pmr::memory_resource& mr) {
  P::SearchResponse resp;
  std::byte rid[] = {std::byte{1}, std::byte{2}, std::byte{3}};
  resp.request_id = B::arenaBytes(mr, rid);
  resp.more = false;

  // ops["results"] -> Val{DocList}
  P::Val* results = B::opsSlot(resp.ops, /*cap*/ 2, "results", mr);
  auto& dl = results->kind.emplace<P::DocList>();
  dl.matches = 42;
  dl.offset = 0;
  dl.more = false;
  dl.max_score = 1.5f;

  // columns["id"] -> ColStr (single-valued string, index fill, arena-backed views)
  {
    P::Column& c = B::columnSlot(dl.columns, /*cap*/ 3, "id", mr);
    auto& cs = c.kind.emplace<P::ColStr>();
    cs.missing_val = "";
    std::string_view* v = B::allocArray(cs.v, 3, mr);
    v[0] = B::arenaStr(mr, "a");
    v[1] = B::arenaStr(mr, "b");
    v[2] = B::arenaStr(mr, "c");
  }
  // columns["price"] -> ColInt (single-valued numeric, missing sentinel)
  {
    P::Column& c = B::columnSlot(dl.columns, 3, "price", mr);
    auto& ci = c.kind.emplace<P::ColInt>();
    ci.missing_val = -1;
    std::int64_t* v = B::allocArray(ci.v, 3, mr);
    v[0] = 10; v[1] = 20; v[2] = 30;
  }
  // columns["tags"] -> ArrArrInt (multi-valued; SpanBuilder per doc; builders die here)
  {
    P::Column& c = B::columnSlot(dl.columns, 3, "tags", mr);
    auto& aa = c.kind.emplace<P::ArrArrInt>();
    P::ArrInt* outer = B::allocArray(aa.v, 3, mr);
    { B::SpanBuilder<std::int64_t> b(mr); b.push_back(1); b.push_back(2); outer[0].v = b.finish(); }
    outer[1].v = {};  // doc1: no tags
    { B::SpanBuilder<std::int64_t> b(mr); b.push_back(9); outer[2].v = b.finish(); }
  }
  // nested sub-facet on the DocList: dl.ops["byCat"] -> Val{int64}
  {
    P::Val* sub = B::opsSlot(dl.ops, /*cap*/ 1, "byCat", mr);
    sub->kind.emplace<std::int64_t>(7);
  }

  // sibling top-level op: ops["stats"] -> Val{double}
  P::Val* stats = B::opsSlot(resp.ops, 2, "stats", mr);
  stats->kind.emplace<double>(3.14);

  return resp;
}

static void verify(const char* phase, const P::SearchResponse& o) {
  CHECK(o.request_id.size() == 3);
  CHECK(o.ops.size() == 2);

  const auto* resultsIv = o.ops.find("results");
  CHECK(resultsIv != nullptr);
  if (!resultsIv) return;
  const P::Val& results = **resultsIv;
  const auto* dlp = std::get_if<P::DocList>(&results.kind);
  CHECK(dlp != nullptr);
  if (!dlp) return;
  const P::DocList& dl = *dlp;
  CHECK(dl.matches && *dl.matches == 42);
  CHECK(dl.max_score && *dl.max_score == 1.5f);
  CHECK(dl.columns.size() == 3);

  const P::Column* id = dl.columns.find("id");
  CHECK(id != nullptr);
  if (id) {
    const auto* cs = std::get_if<P::ColStr>(&id->kind);
    CHECK(cs != nullptr);
    if (cs) { CHECK(cs->v.size() == 3); CHECK(cs->v[1] == "b"sv); }
  }
  const P::Column* price = dl.columns.find("price");
  CHECK(price != nullptr);
  if (price) {
    const auto* ci = std::get_if<P::ColInt>(&price->kind);
    CHECK(ci != nullptr);
    if (ci) { CHECK(ci->v.size() == 3); CHECK(ci->v[2] == 30); CHECK(ci->missing_val == -1); }
  }
  const P::Column* tags = dl.columns.find("tags");
  CHECK(tags != nullptr);
  if (tags) {
    const auto* aa = std::get_if<P::ArrArrInt>(&tags->kind);
    CHECK(aa != nullptr);
    if (aa) {
      CHECK(aa->v.size() == 3);
      CHECK(aa->v[0].v.size() == 2 && aa->v[0].v[1] == 2);
      CHECK(aa->v[1].v.size() == 0);
      CHECK(aa->v[2].v.size() == 1 && aa->v[2].v[0] == 9);
    }
  }
  const auto* byCatIv = dl.ops.find("byCat");
  CHECK(byCatIv != nullptr);
  if (byCatIv) {
    const auto* n = std::get_if<std::int64_t>(&(*byCatIv)->kind);
    CHECK(n != nullptr && *n == 7);
  }

  const auto* statsIv = o.ops.find("stats");
  CHECK(statsIv != nullptr);
  if (statsIv) {
    const auto* d = std::get_if<double>(&(*statsIv)->kind);
    CHECK(d != nullptr && *d == 3.14);
  }
  printf("  %s: verified\n", phase);
}

int main() {
  // 0) in-place (the built object itself, before any serialization)
  {
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    verify("in-place", resp);
  }
  // 1) BINARY round-trip: encode -> decode into a fresh arena
  {
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    std::vector<std::byte> wire;
    bool ok = encode(resp, wire);
    CHECK(ok);
    std::vector<std::byte> pad = wire; pad.push_back(std::byte{0});
    std::span<const std::byte> payload{pad.data(), pad.size() - 1};
    std::pmr::monotonic_buffer_resource arena;
    P::SearchResponse out{};
    CHECK(decode(out, payload, arena));
    verify("binary", out);
  }
  // 2) JSON round-trip: write_json -> read_json into a fresh arena
  {
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    std::string js;
    CHECK(write_json(resp, js));
    std::pmr::monotonic_buffer_resource arena;
    P::SearchResponse out{};
    CHECK(read_json(out, js, arena));
    verify("json", out);
    printf("  json = %s\n", js.c_str());
  }

  if (fails == 0) { printf("build_test: ALL PASS\n"); return 0; }
  printf("build_test: %d FAIL\n", fails);
  return 1;
}
