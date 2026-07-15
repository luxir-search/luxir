// Wire-model regression net for the concrete solux::api classes. Instead of hand-writing an
// instance + a verifier per message (which silently fails to cover any field someone forgets
// to add), this drives EVERY message type through one generic round-trip:
//
//   fill every field with a non-default value (walking glz::meta)  ->  canon = write_json(in)
//   binary:  encode -> decode -> write_json   must equal canon
//   json:    read_json(canon) -> write_json   must equal canon
//
// A canonical-JSON fixpoint catches any field the codec drops or mis-tags, with zero per-field
// code: a newly added proto field is covered the moment it exists in the struct + meta. The one
// hand-maintained thing is the message-type list (SOLUX_MSGS) - adding a message is one line,
// and a missing message is then the single possible coverage gap (vs per-field gaps before).
//
// Scope/limits: this is self-round-trip (fixpoint), so it catches dropped/mis-tagged fields and
// struct<->meta drift, but NOT encode+decode sharing the *same* wrong tag (= wire-incompatible
// with real protobuf); that needs a golden-bytes-vs-protobuf check, which the protobuf/hpp-proto
// TU conflict pushes to a separate harness (TODO). Recursion (Query->ConstantScoreQuery->Query,
// Val->Map->Val) is broken by a depth cap; depth only bounds how deep we exercise - the fixpoint
// is correct at any depth. BuildByBacking (below) stays hand-written: it tests the build.h helper
// API (slots/SpanBuilder/lifetime), not the schema, so an explicit example is the right tool.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"
#include "solux/api/solux.hpp"
#include "solux/api/build.h"

namespace {
using namespace std::string_view_literals;
namespace P = solux::api;
namespace B = solux::api::build;

// Cap on message-nesting depth (breaks self-referential cycles). Coverage only; the fixpoint
// holds at any depth.
constexpr int kMaxDepth = 5;

// ----- member-shape traits (with payload extraction) -----
template <class> struct vt_optional : std::false_type {};
template <class U> struct vt_optional<std::optional<U>> : std::true_type {};
template <class> struct vt_opt_indirect : std::false_type {};
template <class U> struct vt_opt_indirect<::hpp_proto::optional_indirect_view<U>> : std::true_type { using type = U; };
template <class> struct vt_indirect : std::false_type {};
template <class U> struct vt_indirect<::hpp_proto::indirect_view<U>> : std::true_type { using type = U; };
template <class> struct vt_variant : std::false_type {};
template <class... A> struct vt_variant<std::variant<A...>> : std::true_type {};
template <class> struct vt_span : std::false_type {};
template <class U, std::size_t E> struct vt_span<std::span<U, E>> : std::true_type { using type = std::remove_cv_t<U>; };
template <class> struct vt_map : std::false_type {};
template <class K, class V> struct vt_map<solux::api::map_view<K, V>> : std::true_type { using val = V; };

// ----- small helpers -----
template <class U>
U* arenaNew(std::pmr::memory_resource& mr) {
  U* p = (U*)mr.allocate(sizeof(U), alignof(U));
  ::new (p) U();
  return p;
}

// One-entry by-value map slot (for map_view<sv, Column> / map_view<sv, TopDocs>).
template <class V>
V& mapValueSlot(solux::api::map_view<std::string_view, V>& m, std::string_view key,
                std::pmr::memory_resource& mr) {
  using Pair = std::pair<std::string_view, V>;
  Pair* arr = (Pair*)mr.allocate(sizeof(Pair), alignof(Pair));
  ::new (arr) Pair();
  arr->first = key;
  m = solux::api::map_view<std::string_view, V>(std::span<const Pair>(arr, 1));
  return arr->second;
}

// Runtime index -> compile-time: invoke f.operator()<idx>().
template <std::size_t N, class F>
void visitIndex(std::size_t idx, F&& f) {
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (void)((I == idx ? (f.template operator()<I>(), true) : false) || ...);
  }(std::make_index_sequence<N>{});
}

template <class M> void fillMessage(M&, std::pmr::memory_resource&, int depth);
template <class V> void fillVariantArm(V&, std::size_t idx, std::pmr::memory_resource&, int depth);

// Fill one member with a non-default value. A message payload descends via fillMessage(depth+1);
// scalars/strings/bytes fill in place. fillMessage's depth cap is the sole recursion guard.
template <class T>
void fillField(T& x, std::pmr::memory_resource& mr, int depth) {
  if constexpr (std::is_same_v<T, bool>) {
    x = true;
  } else if constexpr (std::is_enum_v<T>) {
    // Any nonzero value round-trips as a fixpoint (glaze writes the enumerator name if known,
    // else the integer); the enum's glz::meta is out-of-line, so we can't reflect names here.
    x = static_cast<T>(1);
  } else if constexpr (std::is_integral_v<T>) {
    x = T(1);
  } else if constexpr (std::is_floating_point_v<T>) {
    x = T(1);
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    x = std::string_view("x");
  } else if constexpr (std::is_same_v<T, ::hpp_proto::bytes_view>) {
    static constexpr std::array<std::byte, 1> one{std::byte{1}};
    x = B::arenaBytes(mr, one);
  } else if constexpr (vt_optional<T>::value) {
    x.emplace();
    fillField(*x, mr, depth);
  } else if constexpr (vt_opt_indirect<T>::value) {
    using U = typename vt_opt_indirect<T>::type;
    U* p = arenaNew<U>(mr);
    fillField(*p, mr, depth);
    x = p;
  } else if constexpr (vt_variant<T>::value) {
    fillVariantArm(x, std::variant_size_v<T> - 1, mr, depth);  // last arm
  } else if constexpr (vt_map<T>::value) {
    using V = typename vt_map<T>::val;
    if constexpr (vt_indirect<V>::value) {
      using U = typename vt_indirect<V>::type;
      U* p = B::mapSlot<U>(x, 1, "k", mr);
      fillField(*p, mr, depth);
    } else {
      V& slot = mapValueSlot(x, "k", mr);
      fillField(slot, mr, depth);
    }
  } else if constexpr (vt_span<T>::value) {
    using U = typename vt_span<T>::type;
    U* a = B::allocArray(x, 2, mr);
    for (int i = 0; i < 2; i++) fillField(a[i], mr, depth);
  } else if constexpr (glz::reflectable<T>) {
    // A message struct. glz::meta is out-of-line (invisible in this TU), so the structs reflect
    // as plain aggregates - glz::reflectable, not glaze_object_t.
    fillMessage(x, mr, depth + 1);
  } else {
    static_assert(sizeof(T) == 0, "fillField: unhandled member type");
  }
}

template <class M>
void fillMessage(M& m, std::pmr::memory_resource& mr, int depth) {
  if (depth > kMaxDepth) return;
  glz::for_each_field(m, [&](auto& mem) { fillField(mem, mr, depth); });
}

template <class V>
void fillVariantArm(V& v, std::size_t idx, std::pmr::memory_resource& mr, int depth) {
  visitIndex<std::variant_size_v<V>>(idx, [&]<std::size_t I>() {
    auto& a = v.template emplace<I>();
    using Arm = std::remove_cvref_t<decltype(a)>;
    if constexpr (!std::is_same_v<Arm, std::monostate>) fillField(a, mr, depth);
  });
}

// variant_size of M's (single) oneof member, or 1 if M has no variant.
template <class M>
std::size_t variantArms() {
  std::size_t n = 1;
  M m{};
  glz::for_each_field(m, [&](auto& mem) {
    using Mem = std::remove_cvref_t<decltype(mem)>;
    if constexpr (vt_variant<Mem>::value) n = std::variant_size_v<Mem>;
  });
  return n;
}

template <class M>
void setArm(M& m, std::size_t idx, std::pmr::memory_resource& mr) {
  glz::for_each_field(m, [&](auto& mem) {
    using Mem = std::remove_cvref_t<decltype(mem)>;
    if constexpr (vt_variant<Mem>::value) fillVariantArm(mem, idx, mr, 1);
  });
}

template <class M, class Setup>
void runOne(const char* nm, std::size_t arm, bool nonEmpty, Setup&& setup) {
  std::pmr::monotonic_buffer_resource src;
  M in{};
  fillMessage(in, src, 1);
  setup(in, src);

  std::string canon;
  ASSERT_TRUE(write_json(in, canon)) << nm << " arm" << arm << " write_json(in)";
  // Guard against a vacuous pass: if the filler silently stopped populating fields, every
  // message would serialize to "{}" and the fixpoint would hold trivially. A populated message
  // (or a non-monostate oneof arm) must produce something (untagged Val scalar arms are as
  // short as "1", so only empty/empty-object trips it).
  if (nonEmpty) {
    EXPECT_TRUE(!canon.empty() && canon != "{}") << nm << " arm" << arm << " filled to empty JSON";
  }
  {  // binary: encode -> decode (payload span with padded backing) -> write_json == canon
    std::vector<std::byte> w;
    ASSERT_TRUE(encode(in, w)) << nm << " arm" << arm << " encode";
    std::pmr::monotonic_buffer_resource a;
    M out{};
    auto padded = P::copyToPaddedInput(std::span<const std::byte>(w), a);
    ASSERT_TRUE(decode(out, padded, a)) << nm << " arm" << arm << " decode";
    std::string j;
    ASSERT_TRUE(write_json(out, j)) << nm << " arm" << arm << " write_json(bin out)";
    EXPECT_EQ(canon, j) << nm << " arm" << arm << " BINARY round-trip mismatch";
  }
  {  // json: read_json(canon) -> write_json == canon
    std::pmr::monotonic_buffer_resource a;
    M out{};
    ASSERT_TRUE(read_json(out, canon, a)) << nm << " arm" << arm << " read_json json=" << canon;
    std::string j;
    ASSERT_TRUE(write_json(out, j)) << nm << " arm" << arm << " write_json(json out)";
    EXPECT_EQ(canon, j) << nm << " arm" << arm << " JSON round-trip mismatch";
  }
}

// Round-trip M: one filled instance, or (for a oneof message) one instance per arm incl.
// monostate, so per-arm tag correctness is covered.
template <class M>
void roundTripType(const char* nm) {
  std::size_t arms = variantArms<M>();
  if (arms <= 1) {
    runOne<M>(nm, 0, /*nonEmpty*/ true, [](M&, std::pmr::memory_resource&) {});
  } else {
    for (std::size_t i = 0; i < arms; i++) {  // arm 0 is std::monostate -> legitimately "{}"
      runOne<M>(nm, i, /*nonEmpty*/ i != 0,
                [i](M& m, std::pmr::memory_resource& mr) { setArm(m, i, mr); });
    }
  }
}

// The one hand-maintained list: every message type. Add a message -> add a line.
#define SOLUX_MSGS(X)                                                                              \
  X(Target) X(SearchRequest) X(SearchOp) X(GenOp) X(TopDocs) X(Fusion) X(RrfFusion) X(SortSpec)    \
  X(Query) X(ExistsQuery) X(ConstantScoreQuery) X(BoostQuery) X(KnnQuery) X(Match) X(NamedQuery)    \
  X(BooleanQuery) X(PrefixQuery) X(FuzzyQuery) X(PhraseQuery) X(GeoBoxQuery) X(GeoDistanceQuery)   \
  X(FieldFacet)                                                                                     \
  X(CalendarGap) X(RangeFacet) X(Domain)                                                            \
  X(SearchResponse) X(DocList) X(FacetResult) X(Bucket) X(CommitParams) X(UpdateRequest)           \
  X(UpdateResponse) X(NamedValue) X(Map) X(Columns) X(Val) X(ArrVal) X(ArrStr) X(ArrInt)           \
  X(ArrFloat) X(ArrDouble) X(ArrBin) X(ArrArrStr) X(ArrArrInt) X(ArrArrFloat) X(ArrArrDouble)      \
  X(ArrArrBin) X(Vector) X(ArrVector) X(ColStr) X(Column) X(ColVector) X(MultiVector) X(ColInt)    \
  X(ColFloat) X(ColDouble) X(ColMap) X(IndexInfo) X(AuxIndexInfo) X(SegmentInfo) X(AnalyzerDef)    \
  X(FieldDef) X(SchemaDef) X(SchemaRequest) X(SchemaResponse)                                      \
  X(HelloRequest) X(HelloReply)

TEST(ProtoRoundTrip, AllMessages) {
#define RT(T) roundTripType<P::T>(#T);
  SOLUX_MSGS(RT)
#undef RT
}

TEST(ProtoRoundTrip, PhraseSlopValues) {
  auto check = [](int32_t value) {
    P::PhraseQuery in;
    in.field = "body_w";
    in.text = "a b";
    in.slop = value;

    std::vector<std::byte> wire;
    ASSERT_TRUE(encode(in, wire));
    std::pmr::monotonic_buffer_resource binaryArena;
    P::PhraseQuery binaryOut;
    auto padded = P::copyToPaddedInput(std::span<const std::byte>(wire), binaryArena);
    ASSERT_TRUE(decode(binaryOut, padded, binaryArena));
    EXPECT_EQ(value, binaryOut.slop);

    std::string json;
    ASSERT_TRUE(write_json(in, json));
    std::pmr::monotonic_buffer_resource jsonArena;
    P::PhraseQuery jsonOut;
    ASSERT_TRUE(read_json(jsonOut, json, jsonArena)) << json;
    EXPECT_EQ(value, jsonOut.slop);
  };

  check(0);
  check(7);
  check(std::numeric_limits<int32_t>::max());
  check(-1);  // The wire accepts it; query validation rejects it.

  std::pmr::monotonic_buffer_resource arena;
  P::PhraseQuery absent;
  ASSERT_TRUE(read_json(absent, R"({"field":"body_w","text":"a b"})", arena));
  EXPECT_EQ(0, absent.slop);
}

// ---- build-by-backing: assemble a non-owning SearchResponse with the build.h helpers, then
// round-trip it. Hand-written on purpose - this tests the build API (slots, allocArray, arenaStr,
// SpanBuilder incl. builder-dies-before-serialize, nested ops), not the schema.

P::SearchResponse buildResponse(std::pmr::memory_resource& mr) {
  P::SearchResponse resp;
  resp.request_id = B::arenaStr(mr, "rid-123");
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

void verifyResponse(const P::SearchResponse& o) {
  EXPECT_EQ(o.request_id, "rid-123");
  ASSERT_EQ(o.ops.size(), 2u);

  const auto* resultsIv = o.ops.find("results");
  ASSERT_NE(resultsIv, nullptr);
  const P::Val& results = **resultsIv;
  const auto* dlp = std::get_if<P::DocList>(&results.kind);
  ASSERT_NE(dlp, nullptr);
  const P::DocList& dl = *dlp;
  EXPECT_TRUE(dl.matches && *dl.matches == 42);
  EXPECT_TRUE(dl.max_score && *dl.max_score == 1.5f);
  ASSERT_EQ(dl.columns.size(), 3u);

  const P::Column* id = dl.columns.find("id");
  ASSERT_NE(id, nullptr);
  { const auto* cs = std::get_if<P::ColStr>(&id->kind);
    ASSERT_NE(cs, nullptr);
    ASSERT_EQ(cs->v.size(), 3u); EXPECT_EQ(cs->v[1], "b"sv); }

  const P::Column* price = dl.columns.find("price");
  ASSERT_NE(price, nullptr);
  { const auto* ci = std::get_if<P::ColInt>(&price->kind);
    ASSERT_NE(ci, nullptr);
    ASSERT_EQ(ci->v.size(), 3u); EXPECT_EQ(ci->v[2], 30); EXPECT_EQ(ci->missing_val, -1); }

  const P::Column* tags = dl.columns.find("tags");
  ASSERT_NE(tags, nullptr);
  { const auto* aa = std::get_if<P::ArrArrInt>(&tags->kind);
    ASSERT_NE(aa, nullptr);
    ASSERT_EQ(aa->v.size(), 3u);
    ASSERT_EQ(aa->v[0].v.size(), 2u); EXPECT_EQ(aa->v[0].v[1], 2);
    EXPECT_EQ(aa->v[1].v.size(), 0u);
    ASSERT_EQ(aa->v[2].v.size(), 1u); EXPECT_EQ(aa->v[2].v[0], 9); }

  const auto* byCatIv = dl.ops.find("byCat");
  ASSERT_NE(byCatIv, nullptr);
  { const auto* n = std::get_if<std::int64_t>(&(*byCatIv)->kind);
    ASSERT_NE(n, nullptr); EXPECT_EQ(*n, 7); }

  const auto* statsIv = o.ops.find("stats");
  ASSERT_NE(statsIv, nullptr);
  { const auto* d = std::get_if<double>(&(*statsIv)->kind);
    ASSERT_NE(d, nullptr); EXPECT_DOUBLE_EQ(*d, 3.14); }
}

TEST(ProtoRoundTrip, BuildByBacking) {
  {  // in-place (the built object itself, before any serialization)
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    verifyResponse(resp);
  }
  {  // binary round-trip (payload span with padded backing)
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    std::vector<std::byte> wire;
    ASSERT_TRUE(encode(resp, wire));
    std::pmr::monotonic_buffer_resource arena;
    P::SearchResponse out{};
    auto padded = P::copyToPaddedInput(std::span<const std::byte>(wire), arena);
    ASSERT_TRUE(decode(out, padded, arena));
    verifyResponse(out);
  }
  {  // json round-trip: the dialect renders Val untagged (a raw JSON value), so the
     // result-only DocList arm reads back as its JSON-native projection (a Map). Assert
     // the text fixpoint plus spot-checks of the projected view.
    std::pmr::monotonic_buffer_resource mr;
    P::SearchResponse resp = buildResponse(mr);
    std::string js;
    ASSERT_TRUE(write_json(resp, js));
    std::pmr::monotonic_buffer_resource arena;
    P::SearchResponse out{};
    ASSERT_TRUE(read_json(out, js, arena)) << "json=" << js;
    std::string js2;
    ASSERT_TRUE(write_json(out, js2));
    EXPECT_EQ(js, js2) << "write(read(js)) fixpoint";

    const auto* resultsIv = out.ops.find("results");
    ASSERT_NE(resultsIv, nullptr);
    const auto* m = std::get_if<P::Map>(&(*resultsIv)->kind);
    ASSERT_NE(m, nullptr);
    const auto* matches = m->fields.find("matches");
    ASSERT_NE(matches, nullptr);
    { const auto* n = std::get_if<std::int64_t>(&(**matches).kind);
      ASSERT_NE(n, nullptr); EXPECT_EQ(*n, 42); }
    const auto* statsIv = out.ops.find("stats");
    ASSERT_NE(statsIv, nullptr);
    { const auto* d = std::get_if<double>(&(*statsIv)->kind);
      ASSERT_NE(d, nullptr); EXPECT_DOUBLE_EQ(*d, 3.14); }
  }
}

}  // namespace
