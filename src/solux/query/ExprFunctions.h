#pragma once

#include <array>
#include <string_view>
#include <utility>
#include <variant>

#include <glaze/reflection/get_name.hpp>
#include <glaze/reflection/to_tuple.hpp>
#include <glaze/tuplet/tuple.hpp>

#include "solux/api/solux_types.hpp"

// Metadata for the expr language's function-call form: every query type is
// callable as name(main value, arg=value, ...), where the function name is the
// Query oneof arm's JSON name and argument names are the message's field
// names.  One vocabulary across protobuf, JSON, and the query string - a new
// proto query type is callable the day it lands, with no parser tables to
// update beyond naming its arm below.
//
// Member names come from glaze pure reflection over the aggregate message
// structs.  That is deliberately meta-independent (member_names / to_tie never
// consult glz::meta, which only the generated metadata TU defines), so the
// names here cannot diverge from the generated JSON dialect; the one
// reserved-word escape (operator_) is undone by jsonName().

namespace solux::expr {

// JSON names of the api::Query oneof arms, ordered by variant index (= proto
// field order).  The static_assert keeps this table in lockstep with the
// variant: a new arm fails to compile until it is named here (and its expr
// callability decided).
inline constexpr std::array<std::string_view, 16> ARM_NAMES = {
    "",               // monostate (unset)
    "match",          // Match
    "boolean",        // BooleanQuery
    "all",            // bool - callable as all(), handled specially
    "exists",         // ExistsQuery
    "phrase",         // PhraseQuery
    "knn",            // KnnQuery
    "constant_score", // ConstantScoreQuery
    "prefix",         // PrefixQuery
    "fuzzy",          // FuzzyQuery
    "simple_query",   // SimpleQuery
    "range",          // RangeQuery
    "expr",           // ExprQuery - not callable within expr (write it inline)
    "geo_box",        // GeoBoxQuery - not callable in this pass
    "geo_distance",   // GeoDistanceQuery - not callable in this pass
    "boost",          // BoostQuery
};
static_assert(std::variant_size_v<decltype(api::Query::kind)> == ARM_NAMES.size(),
              "Query gained an arm: name it in ARM_NAMES and decide its expr callability");

// The one positional argument a function accepts: its "main value" (the same
// role as Solr local-params' v).  This is DECLARED knowledge, standing in for
// a json_name-style custom proto option until the generator grows one, and is
// deliberately the only per-function table in the language.  Empty = named
// arguments only.  The parse form follows the member's TYPE: raw text for
// string/Val slots, a sub-expression for Query slots.
inline constexpr std::string_view mainValueArg(std::string_view fn) {
  if (fn == "match") return "val";
  if (fn == "exists") return "field";
  if (fn == "phrase") return "text";
  if (fn == "simple_query") return "q";
  if (fn == "prefix") return "prefix";
  if (fn == "fuzzy") return "term";
  if (fn == "constant_score") return "query";
  if (fn == "boost") return "query";
  return {};
}

// The JSON/proto name of a C++ member: strip the trailing '_' some members
// carry to dodge C++ keywords (operator_ -> "operator").
inline constexpr std::string_view jsonName(std::string_view member) {
  if (!member.empty() && member.back() == '_') member.remove_suffix(1);
  return member;
}

// Emplace the message arm named `name` in q and invoke f on the fresh
// message.  Returns false when no callable message arm has that name; the
// caller owns the error (and the special cases: "all" is the bool arm and
// "expr" is deliberately not callable).
template <typename F>
bool withCallableArm(api::Query& q, std::string_view name, F&& f) {
  bool called = false;
  auto tryArm = [&]<size_t I>() {
    using Arm = std::variant_alternative_t<I, decltype(api::Query::kind)>;
    if constexpr (std::is_class_v<Arm> && !std::is_same_v<Arm, std::monostate> &&
                  !std::is_same_v<Arm, api::ExprQuery> &&
                  !std::is_same_v<Arm, api::GeoBoxQuery> &&
                  !std::is_same_v<Arm, api::GeoDistanceQuery>) {
      if (!called && name == ARM_NAMES[I]) {
        called = true;
        f(q.kind.template emplace<I>());
      }
    }
  };
  [&]<size_t... I>(std::index_sequence<I...>) {
    (tryArm.template operator()<I>(), ...);
  }(std::make_index_sequence<ARM_NAMES.size()>{});
  return called;
}

// Invoke f(memberRef) on the member of `arm` whose JSON name is `name`.
// Returns false when no member matches.
template <typename Arm, typename F>
bool withMember(Arm& arm, std::string_view name, F&& f) {
  constexpr auto& names = glz::member_names<Arm>;
  auto tie = glz::to_tie(arm);
  bool found = false;
  auto tryMember = [&]<size_t I>() {
    if (!found && jsonName(names[I]) == name) {
      found = true;
      f(glz::get<I>(tie));
    }
  };
  [&]<size_t... I>(std::index_sequence<I...>) {
    (tryMember.template operator()<I>(), ...);
  }(std::make_index_sequence<names.size()>{});
  return found;
}

} // namespace solux::expr
