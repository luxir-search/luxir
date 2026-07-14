#pragma once
// Arena-backed builders for the concrete solux::api query/op constructs the OpCursor fluent
// helpers don't cover (boolean trees, knn, fusion sources, sorts). Every builder allocates
// nested spans into a caller-provided std::pmr::memory_resource (use OpCursor::mr() / the
// LocalReq's `mr`), which must outlive the request. All solux::api types are trivially
// copyable with their nested data in the arena, so Query/SortSpec values can be passed and
// copied by value freely.
//
// Typical use:
//   auto& cur = req->topDocs("q");
//   cur.rawQuery() = qb::boolean(cur.mr(), {qb::match(cur.mr(), "body_w", "apple")}, {}, {},
//                                {qb::prefix(cur.mr(), "color_s", "re")});
//   qb::sort(cur, "name_s", qb::ASC);

#include <initializer_list>
#include <span>
#include <vector>

#include "LocalReq.h"
#include "solux/api/build.h"
#include "solux/api/solux_types.hpp"

namespace solux::test::qb {

namespace api = solux::api;
namespace build = solux::api::build;

using SortDir = api::SortSpec_::SortDir;
inline constexpr SortDir ASC = SortDir::ASC;
inline constexpr SortDir DESC = SortDir::DESC;
using MatchOp = api::Match_::Operator;

// ---- leaf / wrapper Query builders (return a Query by value; nested data in `mr`) ----

inline api::Query all() {
  api::Query q;
  q.kind = true;  // the `all` arm
  return q;
}

inline api::Query match(std::pmr::memory_resource& mr, std::string_view field,
                        std::string_view value, MatchOp op = MatchOp::OPERATOR_UNSPECIFIED) {
  api::Query q;
  auto& m = q.kind.emplace<api::Match>();
  m.field = build::arenaStr(mr, field);
  auto* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
  new (v) api::Val();
  v->kind = build::arenaStr(mr, value);
  m.val = v;
  m.operator_ = op;
  return q;
}

inline api::Query prefix(std::pmr::memory_resource& mr, std::string_view field,
                         std::string_view prefix) {
  api::Query q;
  auto& p = q.kind.emplace<api::PrefixQuery>();
  p.field = build::arenaStr(mr, field);
  p.prefix = build::arenaStr(mr, prefix);
  return q;
}

// ---- range bound Vals: allocate a Val with one scalar arm in the arena ----
inline api::Val* valI64(std::pmr::memory_resource& mr, int64_t x) {
  auto* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
  new (v) api::Val();
  v->kind = x;
  return v;
}
inline api::Val* valF64(std::pmr::memory_resource& mr, double x) {
  auto* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
  new (v) api::Val();
  v->kind = x;
  return v;
}
inline api::Val* valStr(std::pmr::memory_resource& mr, std::string_view s) {
  auto* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
  new (v) api::Val();
  v->kind = build::arenaStr(mr, s);
  return v;
}

// RangeQuery with explicit bound Vals (any may be nullptr for an open side).
// Build bounds with valI64 / valF64 / valStr.  Set at most one of gte/gt and
// one of lte/lt (the builder errors otherwise).
inline api::Query range(std::pmr::memory_resource& mr, std::string_view field,
                        api::Val* gte, api::Val* gt, api::Val* lte, api::Val* lt) {
  api::Query q;
  auto& r = q.kind.emplace<api::RangeQuery>();
  r.field = build::arenaStr(mr, field);
  if (gte) r.gte = gte;
  if (gt)  r.gt  = gt;
  if (lte) r.lte = lte;
  if (lt)  r.lt  = lt;
  return q;
}

inline api::Query geoBox(std::pmr::memory_resource& mr, std::string_view field,
                         double minLat, double maxLat, double minLon,
                         double maxLon) {
  api::Query q;
  auto& g = q.kind.emplace<api::GeoBoxQuery>();
  g.field = build::arenaStr(mr, field);
  g.min_lat = minLat;
  g.max_lat = maxLat;
  g.min_lon = minLon;
  g.max_lon = maxLon;
  return q;
}

inline api::Query geoDistance(std::pmr::memory_resource& mr,
                              std::string_view field, double lat, double lon,
                              double radiusMeters) {
  api::Query q;
  auto& g = q.kind.emplace<api::GeoDistanceQuery>();
  g.field = build::arenaStr(mr, field);
  g.lat = lat;
  g.lon = lon;
  g.radius_meters = radiusMeters;
  return q;
}

inline api::Query fuzzy(std::pmr::memory_resource& mr, std::string_view field,
                        std::string_view term, int maxEdits = -1, int prefixLength = -1,
                        int maxExpansions = 0) {
  api::Query q;
  auto& f = q.kind.emplace<api::FuzzyQuery>();
  f.field = build::arenaStr(mr, field);
  f.term = build::arenaStr(mr, term);
  if (maxEdits >= 0) f.max_edits = maxEdits;
  if (prefixLength >= 0) f.prefix_length = prefixLength;
  if (maxExpansions > 0) f.max_expansions = maxExpansions;
  return q;
}

inline api::Query phraseWords(std::pmr::memory_resource& mr, std::string_view field,
                              std::initializer_list<std::string_view> words) {
  api::Query q;
  auto& p = q.kind.emplace<api::PhraseQuery>();
  p.field = build::arenaStr(mr, field);
  std::string_view* a = build::allocArray(p.words, words.size(), mr);
  std::size_t i = 0;
  for (auto w : words) a[i++] = build::arenaStr(mr, w);
  return q;
}

inline api::Query phraseText(std::pmr::memory_resource& mr, std::string_view field,
                             std::string_view text) {
  api::Query q;
  auto& p = q.kind.emplace<api::PhraseQuery>();
  p.field = build::arenaStr(mr, field);
  p.text = build::arenaStr(mr, text);
  return q;
}

inline api::Query knn(std::pmr::memory_resource& mr, std::string_view field,
                      std::span<const float> vec, int k, int nprobe = 0, bool exact = false,
                      int refineCandidates = 0, float minScanFraction = 0) {
  api::Query q;
  auto& kq = q.kind.emplace<api::KnnQuery>();
  kq.field = build::arenaStr(mr, field);
  auto& v = kq.query.emplace();
  auto& f32 = v.f32.emplace();
  float* a = build::allocArray(f32.v, vec.size(), mr);
  std::copy(vec.begin(), vec.end(), a);
  kq.k = k;
  kq.nprobe = nprobe;
  kq.exact = exact;
  kq.refine_candidates = refineCandidates;
  kq.min_scan_fraction = minScanFraction;
  return q;
}
inline api::Query knn(std::pmr::memory_resource& mr, std::string_view field,
                      std::initializer_list<float> vec, int k, int nprobe = 0,
                      bool exact = false, int refineCandidates = 0, float minScanFraction = 0) {
  return knn(mr, field, std::span<const float>(vec.begin(), vec.size()), k, nprobe, exact,
             refineCandidates, minScanFraction);
}

inline api::Query constantScore(std::pmr::memory_resource& mr, const api::Query& inner,
                                std::optional<float> score = std::nullopt) {
  api::Query q;
  auto& c = q.kind.emplace<api::ConstantScoreQuery>();
  auto* p = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
  new (p) api::Query(inner);
  c.query = p;
  c.score = score;
  return q;
}

inline api::Query boost(std::pmr::memory_resource& mr, const api::Query& inner,
                        std::optional<float> factor = std::nullopt) {
  api::Query q;
  auto& b = q.kind.emplace<api::BoostQuery>();
  auto* p = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
  new (p) api::Query(inner);
  b.query = p;
  b.boost = factor;
  return q;
}

// ---- boolean: copy the clause lists into arena spans ----
inline void setSpan(std::span<const api::Query>& target, std::pmr::memory_resource& mr,
                    std::span<const api::Query> src) {
  if (src.empty()) { target = {}; return; }
  api::Query* a = build::allocArray(target, src.size(), mr);
  std::copy(src.begin(), src.end(), a);
}

inline api::Query boolean(std::pmr::memory_resource& mr, std::span<const api::Query> required,
                          std::span<const api::Query> optional = {},
                          std::span<const api::Query> prohibited = {},
                          std::span<const api::Query> filter = {}, int minMatch = 0) {
  api::Query q;
  auto& b = q.kind.emplace<api::BooleanQuery>();
  setSpan(b.required, mr, required);
  setSpan(b.optional, mr, optional);
  setSpan(b.prohibited, mr, prohibited);
  setSpan(b.filter, mr, filter);
  b.min_match = minMatch;
  return q;
}
inline api::Query boolean(std::pmr::memory_resource& mr,
                          std::initializer_list<api::Query> required,
                          std::initializer_list<api::Query> optional = {},
                          std::initializer_list<api::Query> prohibited = {},
                          std::initializer_list<api::Query> filter = {}, int minMatch = 0) {
  return boolean(mr, std::span<const api::Query>(required.begin(), required.size()),
                 std::span<const api::Query>(optional.begin(), optional.size()),
                 std::span<const api::Query>(prohibited.begin(), prohibited.size()),
                 std::span<const api::Query>(filter.begin(), filter.size()), minMatch);
}

// ---- TopDocs / facet field setters via an OpCursor ----

// Set the cursor op's query to a pre-built Query.
inline OpCursor& setQuery(OpCursor& cur, const api::Query& q) {
  cur.rawQuery() = q;
  return cur;
}

// Append a sort spec to the cursor op (TopDocs / FieldFacet / RangeFacet).
inline OpCursor& sort(OpCursor& cur, std::string_view field, SortDir dir) {
  auto& mr = cur.mr();
  std::span<const api::SortSpec>* sorts = nullptr;
  std::visit([&](auto& op) {
    if constexpr (requires { op.sorts; }) sorts = &op.sorts;
  }, cur.rawOp().kind);
  assert(sorts && "sort() on an op without sorts");
  auto old = *sorts;
  api::SortSpec* a = build::allocArray(*sorts, old.size() + 1, mr);
  for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
  a[old.size()].field = build::arenaStr(mr, field);
  a[old.size()].dir = dir;
  return cur;
}

}  // namespace solux::test::qb
