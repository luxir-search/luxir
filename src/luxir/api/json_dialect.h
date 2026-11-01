// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Luxir JSON dialect: hand-written glaze from/to specializations that override the
// generated glz::meta where the mechanical proto mapping is not the JSON we want.
// This file is the single home for JSON *shape* customization; key naming stays
// declarative in the .proto (json_name / generator options).
//
// How overriding works: glaze dispatches serialization through glz::from<JSON,T> /
// glz::to<JSON,T>. The generated metadata drives constrained partial specializations,
// so an explicit full specialization here always wins. Most glaze code is instantiated in
// the generated luxir_types.json.cpp TU (via the read_json/write_json entry points), which
// reaches this header through luxir_types_json.hpp - so an override here applies everywhere,
// consistently.
//
// Included by luxir_types_json.hpp (after all message definitions); do not include directly.
//
// Dialect decisions encoded here:
// - Val is a raw JSON value (like google.protobuf.Value's canonical mapping), not a
//   tagged oneof object: writes are untagged for every arm; reads dispatch on the JSON
//   token. Read maps numbers to i (integral token) or d, arrays to the most specific
//   homogeneous arm (ArrStr/ArrInt/ArrDouble, else ArrVal), objects to Map. The
//   engine-only arms are therefore write-normalizing: f reads back as d, bin (base64
//   string) as s, vec as ArrDouble/ArrInt, docs/facet as Map. write(read(json)) is
//   text-identity; read(write(val)) may normalize the arm.
// - Map is a plain JSON object (the wrapper message adds no key level). Duplicate
//   keys are kept in wire order and reads resolve last-wins (map_view::find scans
//   backward), matching protobuf's map semantics on the binary wire.
// - Vector is a bare number array (null when no encoding is set).
// - Match reads accept sugar: {"<field>": <value>} - a single unknown key acts as
//   field+val and composes with operator/min_match; combining it with an explicit
//   "field"/"val" (or a second unknown key) is an error. A schema field literally
//   named like a canonical key needs the canonical form. Writes are always canonical.
// - Query reads accept a bare STRING anywhere a query object goes: it is sugar for
//   the expr arm ({"query": "status:active AND year:>=1960"}), which also gives
//   Filter string sugar. Filter objects without except_ops are structured queries;
//   routed filters use exactly {query, except_ops}, with a nonempty string array.
//   ExprQuery itself reads a bare string as its q
//   ({"expr": "..."} == {"expr": {"q": "..."}}). A Query object takes exactly
//   ONE arm key (proto3 canonical JSON; a second arm is an error, not last-wins).
//   A numeric "boost" sibling wraps that arm in BoostQuery; an object-valued
//   "boost" is the BoostQuery arm itself. Writes stay canonical, so echo mode
//   shows the wrapper rather than the sibling sugar.
// - FieldDef reads accept a bare STRING as type-only sugar: {"year": "int"} ==
//   {"year": {"type": "int"}} in fields, templates, and variants. Writes stay
//   canonical (the object form). A type name that is not built in names a
//   plugin field type, with an optional "params" sibling: {"type": "pair",
//   "params": {...}} == {"plugin_type": {"name": "pair", "params": {...}}},
//   which is what writes produce.
// - FieldVariants and NormalizerDef flatten their presence wrappers to a plain
//   label map and filter array. Empty containers retain presence on the wire.
// - AnalyzerComponent (an AnalyzerDef tokenizer or filter) reads accept a bare
//   STRING as name-only sugar: "lowercase" == {"name": "lowercase"}. Unlike the
//   other sugars, writes use the bare string whenever params is empty, so the
//   common parameterless chain echoes exactly as written; only a component with
//   params writes the object form.
// - AnalyzerDef reads accept a bare STRING as named-analyzer sugar: "english" ==
//   {"name": "english"}. Writes stay canonical (the object form).
// - SortSpec reads a bare STRING as a clause, "expr", "expr asc", or "expr desc"
//   (the HTTP sort parameter grammar), and `field` as an alias for `expr`. A
//   top-docs `sort` takes one clause or a list of them.
//   Writes stay canonical with `expr` because sort expressions are the underlying API.
// - WeightedField reads a canonical object or a bare STRING, "name" or
//   "name^weight". Writes stay canonical with field and optional weight.
// - A Query or SearchOp arm key that names a registered plugin query type or
//   operation (api/PluginArms.h) reads as the plugin arm: {"<name>": {...}} ==
//   {"plugin": {"name": "<name>", "params": {...}}}, and for an operation the
//   object's "ops" member is its nested ops. Writes stay canonical (the
//   plugin object), which reads back the same way.
// - QueryFacet buckets are a name -> Query object on JSON reads and writes. The wire's
//   repeated QueryBucket form preserves that object's document order for gRPC parity.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <unordered_set>

#include "luxir/api/PluginArms.h"
#include "luxir/api/RequestFlags.h"
#include "luxir/api/sort_clause.h"
#include "luxir/api/weighted_field.h"

// NOTE: relies on luxir_types_json.hpp having defined the luxir::api types and included
// <hpp_proto/json.hpp>; kept as a separate file only so JSON-dialect code has one home.
// Include luxir_types_json.hpp, never this directly.

namespace luxir::api::jsond {
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// One JSON number token, parsed in a single pass: an integral token (no fraction or
// exponent) into `i`, anything else into `d`.
struct Number {
  const char* next;  // first byte after the token
  bool isInt;
  std::int64_t i;
  double d;
};

// Parses the number at `p` with glaze's strict JSON grammar and its fast_float
// conversion (the code glaze's own double read runs), so values are bit-identical to
// glaze's per-type reads. Returns false for a malformed token ("01", "1.", ".5", "+1",
// "NaN"), an integral token outside int64, or a double conversion error. Bytes after
// the token are the caller's to validate ("1e" parses 1 and leaves "e").
template <bool NullTerminated>
inline bool parseNumber(const char* p, const char* end, Number& out) {
  auto pns = glz::parse_number_string<NullTerminated>(p, end);
  if (!pns.valid) [[unlikely]] return false;
  out.next = pns.lastmatch;
  out.isInt = pns.lastmatch == pns.integer.ptr + pns.integer.len();
  if (out.isInt) {
    // <= 19 digits cannot wrap the uint64 mantissa; INT64_MIN's magnitude is one past INT64_MAX.
    std::uint64_t limit = (std::uint64_t)INT64_MAX + (pns.negative ? 1 : 0);
    if (pns.integer.len() > 19 || pns.mantissa > limit) [[unlikely]] return false;
    out.i = pns.negative ? (std::int64_t)(0 - pns.mantissa) : (std::int64_t)pns.mantissa;
    return true;
  }
  return glz::fast_float::from_chars_advanced(pns, out.d).ec == std::errc();
}

// Reads an array whose elements are all JSON numbers (`it` at its '[') straight into a
// typed ArrInt or ArrDouble: one pass per number into one buffer, no Val per element.
// Ints stay exact until the first non-integral token, then promote as (double)int64.
// Returns false, with `it` and ctx unchanged, for any other array or any syntax error;
// the caller's generic read then produces the ArrVal/ArrStr or the error. That read
// parses each number with the same parseNumber, so every all-number array it accepts
// is taken here first and it never yields ArrInt/ArrDouble itself.
// Not inlined: the stack buffer must stay out of the recursive Val reader's frame.
template <auto Opts>
[[gnu::noinline]] bool readNumberArray(Val& value, auto& ctx, auto& it, auto& end) {
  static constexpr std::size_t STACK_ELEMS = 1024;
  // Storage for int64s while every element is integral, then (converted in place) doubles.
  alignas(8) std::byte stackBuf[STACK_ELEMS * 8];
  void* buf = stackBuf;
  std::size_t cap = STACK_ELEMS, n = 0;
  bool allInt = true;
  auto& mr = ctx.memory_resource();
  auto bail = [&] {
    ctx.error = glz::error_code::none;  // skip_ws flags running off the input
    return false;
  };
  const char* p = it + 1;
  const char* e = end;
  if (glz::skip_ws<Opts>(ctx, p, e)) return bail();
  while (true) {
    Number num;
    if (!parseNumber<Opts.null_terminated>(p, e, num)) return bail();
    if (n == cap) [[unlikely]] {
      void* grown = mr.allocate(2 * cap * 8, 8);
      std::memcpy(grown, buf, n * 8);
      buf = grown;
      cap *= 2;
    }
    if (allInt && !num.isInt) {
      allInt = false;
      for (std::size_t k = 0; k < n; k++) ((double*)buf)[k] = (double)((std::int64_t*)buf)[k];
    }
    if (allInt) ((std::int64_t*)buf)[n++] = num.i;
    else ((double*)buf)[n++] = num.isInt ? (double)num.i : num.d;
    p = num.next;
    if (glz::skip_ws<Opts>(ctx, p, e)) return bail();
    if (*p == ']') break;
    if (*p != ',') return bail();
    ++p;
    if (glz::skip_ws<Opts>(ctx, p, e)) return bail();
  }
  if (buf == stackBuf) buf = std::memcpy(mr.allocate(n * 8, 8), stackBuf, n * 8);
  if (allInt) value.kind.template emplace<ArrInt>().v = {(const std::int64_t*)buf, n};
  else value.kind.template emplace<ArrDouble>().v = {(const double*)buf, n};
  it = p + 1;  // past ']'
  return true;
}
} // namespace luxir::api::jsond

namespace glz {

template <auto Opts, typename Message>
void readExprVarsSugar(Message &value, std::string_view &expression,
                       std::string_view expressionKey,
                       hpp_proto::concepts::is_non_owning_context auto &ctx,
                       auto &it, auto &end) {
  if constexpr (!check_ws_handled(Opts)) {
    if (skip_ws<Opts>(ctx, it, end)) return;
  }
  static constexpr auto O = ws_handled<Opts>();
  if ((char)*it == '"') {
    util::from_json<O>(expression, ctx, it, end);
    return;
  }
  static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
  std::string_view key;
  decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
  util::scan_object_fields<O, true>(
      ctx, it, end, keyTarget, [](auto &, auto &) {},
      [&](auto &vit, auto &vend) {
        if (key == expressionKey) {
          util::from_json<V>(expression, ctx, vit, vend);
        } else if (key == "vars") {
          decltype(auto) vars =
              ::hpp_proto::detail::as_modifiable(ctx, value.vars);
          glz::util::parse_repeated<V>(true, vars, ctx, vit, vend);
        } else {
          ctx.error = error_code::unknown_key;
          return true;
        }
        return bool(ctx.error);
      },
      [](auto &, auto &) {});
}

// ----- Map: a plain JSON object (no "fields" wrapper key) -----
template <>
struct from<JSON, luxir::api::Map> {
  template <auto Opts>
  static void op(luxir::api::Map &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, value.fields);
    glz::util::parse_repeated<Opts>(true, fields, ctx, it, end);
  }
};

template <>
struct to<JSON, luxir::api::Map> {
  static constexpr bool can_error = true; // Val->Map->Val recursion: don't derive constexpr
  template <auto Opts>
  static void op(const luxir::api::Map &value, is_context auto &ctx, auto &b, auto &ix) {
    // map_view is a range of pairs; glaze writes it as a JSON object.
    serialize<JSON>::template op<Opts>(value.fields, ctx, b, ix);
  }
};

// ----- Vector: a bare number array (null when no encoding present) -----
template <>
struct from<JSON, luxir::api::Vector> {
  template <auto Opts>
  static void op(luxir::api::Vector &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    if (!util::parse_null<Opts>(value.f32, ctx, it, end)) {
      decltype(auto) v = ::hpp_proto::detail::as_modifiable(ctx, value.f32.emplace().v);
      glz::util::parse_repeated<Opts>(false, v, ctx, it, end);
    }
  }
};

template <>
struct to<JSON, luxir::api::Vector> {
  template <auto Opts>
  static void op(const luxir::api::Vector &value, is_context auto &ctx, auto &b, auto &ix) {
    if (value.f32.has_value()) {
      serialize<JSON>::template op<Opts>(value.f32->v, ctx, b, ix);
    } else {
      dump<not check_write_unchecked(Opts)>("null", b, ix);
    }
  }
};

// ----- Match: canonical members plus {"<field>": <value>} sugar on read -----
template <>
struct from<JSON, luxir::api::Match> {
  template <auto Opts>
  static void op(luxir::api::Match &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    bool sawField = false, sawVal = false, sawSugar = false;
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "field") {
            if (sawSugar) {
              ctx.error = error_code::unknown_key;
              return true;
            }
            sawField = true;
            util::from_json<O>(value.field, ctx, vit, vend);
          } else if (key == "val") {
            if (sawSugar) {
              ctx.error = error_code::unknown_key;
              return true;
            }
            sawVal = true;
            from<JSON, ::hpp_proto::optional_indirect_view<api::Val>>::template op<O>(value.val, ctx,
                                                                                      vit, vend);
          } else if (key == "operator") {
            util::from_json<O>(value.operator_, ctx, vit, vend);
          } else if (key == "min_match") {
            util::from_json<O>(value.min_match, ctx, vit, vend);
          } else if (key == "case_insensitive") {
            util::from_json<O>(value.case_insensitive, ctx, vit, vend);
          } else {
            // Sugar: one unknown key acts as field+val. Transfer its arena
            // storage so the next option key cannot overwrite the field name.
            if (sawSugar || sawField || sawVal) {
              ctx.error = error_code::unknown_key;
              return true;
            }
            sawSugar = true;
            value.field = key;
            keyTarget.clear();
            from<JSON, ::hpp_proto::optional_indirect_view<api::Val>>::template op<O>(value.val, ctx,
                                                                                      vit, vend);
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- ExprQuery: bare string = q-only sugar -----
template <>
struct from<JSON, luxir::api::ExprQuery> {
  template <auto Opts>
  static void op(luxir::api::ExprQuery &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    readExprVarsSugar<Opts>(value, value.q, "q", ctx, it, end);
  }
};

// ----- ExprOp: bare string = expr-only sugar -----
template <>
struct from<JSON, luxir::api::ExprOp> {
  template <auto Opts>
  static void op(luxir::api::ExprOp &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    readExprVarsSugar<Opts>(value, value.expr, "expr", ctx, it, end);
  }
};

// ----- Domain: strict keys, with bare Query sugar in filter -----
template <>
struct from<JSON, luxir::api::Domain> {
  template <auto Opts>
  static void op(luxir::api::Domain &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "query") {
            from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
                value.query, ctx, vit, vend);
          } else if (key == "apply_parent_filters") {
            util::from_json<O>(value.apply_parent_filters, ctx, vit, vend);
          } else if (key == "filter") {
            decltype(auto) filters =
                ::hpp_proto::detail::as_modifiable(ctx, value.filter);
            glz::util::parse_repeated<O>(false, filters, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- QueryFacet: buckets are an ordered JSON object, not a repeated array -----
template <auto Opts>
void readQueryFacetBuckets(
    std::span<const luxir::api::QueryBucket> &value,
    hpp_proto::concepts::is_non_owning_context auto &ctx,
    auto &it, auto &end) {
  namespace api = luxir::api;
  static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
  std::string_view name;
  decltype(auto) nameTarget = ::hpp_proto::detail::as_modifiable(ctx, name);
  decltype(auto) buckets = ::hpp_proto::detail::as_modifiable(ctx, value);
  std::unordered_set<std::string_view> names;
  util::scan_object_fields<Opts, true>(
      ctx, it, end, nameTarget, [](auto &, auto &) {},
      [&](auto &vit, auto &vend) {
        if (name.empty()) {
          ctx.error = error_code::syntax_error;
          ctx.custom_error_message = "query_facet bucket names must be nonempty";
          return true;
        }
        size_t index = buckets.size();
        buckets.resize(index + 1);
        char* stored = (char*)ctx.memory_resource().allocate(name.size(), 1);
        std::memcpy(stored, name.data(), name.size());
        buckets[index].name = std::string_view(stored, name.size());
        if (!names.insert(buckets[index].name).second) {
          ctx.error = error_code::syntax_error;
          ctx.custom_error_message = "duplicate query_facet bucket name";
          return true;
        }
        from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
            buckets[index].query, ctx, vit, vend);
        return bool(ctx.error);
      },
      [](auto &, auto &) {});
  if (!bool(ctx.error) && buckets.empty()) {
    ctx.error = error_code::syntax_error;
    ctx.custom_error_message = "query_facet buckets must be nonempty";
  }
}

template <>
struct from<JSON, luxir::api::QueryFacet> {
  template <auto Opts>
  static void op(luxir::api::QueryFacet &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    bool sawBuckets = false;
    bool sawOps = false;
    bool sawSelected = false;
    bool sawSelectionMode = false;
    bool sawCountUnit = false;
    bool sawCountDistinct = false;
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "buckets" && !sawBuckets) {
            sawBuckets = true;
            readQueryFacetBuckets<O>(value.buckets, ctx, vit, vend);
          } else if (key == "ops" && !sawOps) {
            sawOps = true;
            decltype(auto) ops =
                ::hpp_proto::detail::as_modifiable(ctx, value.ops);
            glz::util::parse_repeated<O>(true, ops, ctx, vit, vend);
          } else if (key == "selected" && !sawSelected) {
            sawSelected = true;
            from<JSON, ::hpp_proto::optional_indirect_view<api::Val>>::template op<O>(
                value.selected, ctx, vit, vend);
          } else if (key == "selection_mode" && !sawSelectionMode) {
            sawSelectionMode = true;
            util::from_json<O>(value.selection_mode, ctx, vit, vend);
          } else if (key == "count_unit" && !sawCountUnit) {
            sawCountUnit = true;
            util::from_json<O>(value.count_unit, ctx, vit, vend);
          } else if (key == "count_distinct" && !sawCountDistinct) {
            sawCountDistinct = true;
            util::from_json<O>(value.count_distinct, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
    if (!bool(ctx.error) && !sawBuckets) {
      ctx.error = error_code::missing_key;
      ctx.custom_error_message = "query_facet requires buckets";
    }
  }
};

template <>
struct to<JSON, luxir::api::QueryFacet> {
  template <auto Opts, class B>
  static void op(const luxir::api::QueryFacet &value, is_context auto &ctx,
                 B &b, auto &ix) noexcept {
    dump<"{\"buckets\":{" >(b, ix);
    for (size_t i = 0; i < value.buckets.size(); i++) {
      if (i != 0) dump<','>(b, ix);
      serialize<JSON>::template op<Opts>(value.buckets[i].name, ctx, b, ix);
      dump<':'>(b, ix);
      if (value.buckets[i].query.has_value()) {
        serialize<JSON>::template op<Opts>(
            *value.buckets[i].query, ctx, b, ix);
      } else {
        dump<"null">(b, ix);
      }
    }
    dump<'}'>(b, ix);
    if (!value.ops.empty()) {
      dump<",\"ops\":" >(b, ix);
      serialize<JSON>::template op<Opts>(value.ops, ctx, b, ix);
    }
    if (value.selected.has_value()) {
      dump<",\"selected\":" >(b, ix);
      serialize<JSON>::template op<Opts>(*value.selected, ctx, b, ix);
    }
    if (value.selection_mode != luxir::api::SelectionMode::ANY) {
      dump<",\"selection_mode\":" >(b, ix);
      serialize<JSON>::template op<Opts>(value.selection_mode, ctx, b, ix);
    }
    if (value.count_unit) {
      dump<",\"count_unit\":" >(b, ix);
      serialize<JSON>::template op<Opts>(*value.count_unit, ctx, b, ix);
    }
    if (value.count_distinct) {
      dump<",\"count_distinct\":" >(b, ix);
      serialize<JSON>::template op<Opts>(*value.count_distinct, ctx, b, ix);
    }
    dump<'}'>(b, ix);
  }
};

// Packed option words have one JSON representation at both overriding levels.
// Keep the generated field metadata for ordinary fields so new request options
// cannot silently disappear from canonical echo.
template <auto Opts>
void readRequestFlags(auto& value, auto& ctx, auto& it, auto& end) {
  std::span<const std::string_view> names;
  decltype(auto) target = ::hpp_proto::detail::as_modifiable(ctx, names);
  util::parse_repeated<Opts>(false, target, ctx, it, end);
  if (bool(ctx.error)) return;
  try {
    auto flags = luxir::RequestFlags::fromNames(names, luxir::RequestFlags::levelOf(value));
    value.flags = flags.flags;
    value.flags_set = flags.flagsSet;
  } catch (const std::exception& error) {
    ctx.error = error_code::syntax_error;
    std::string_view message(error.what());
    char* stored = (char*)ctx.memory_resource().allocate(message.size(), 1);
    std::memcpy(stored, message.data(), message.size());
    ctx.custom_error_message = std::string_view(stored, message.size());
  }
}

template <class T>
struct FlaggedRequestJsonWriter {
  static constexpr bool can_error = true;
  template <auto Opts, class B>
  static void op(const T& value, is_context auto& ctx, B& b, auto& ix) {
    if (!luxir::RequestFlags::validationError(value.flags, value.flags_set, luxir::RequestFlags::levelOf(value)).empty()) {
      ctx.error = error_code::syntax_error;
      return;
    }
    dump<'{'>(b, ix);
    bool first = true;
    auto emit = [&](std::string_view key, const auto& member) {
      if (!first) dump<','>(b, ix);
      first = false;
      serialize<JSON>::template op<Opts>(key, ctx, b, ix);
      dump<':'>(b, ix);
      serialize<JSON>::template op<Opts>(member, ctx, b, ix);
    };
    for_each<reflect<T>::size>([&]<size_t I>() {
      constexpr auto key = get<I>(reflect<T>::keys);
      if constexpr (key == "flags_set") {
        return;
      } else if constexpr (key == "flags") {
        auto names = luxir::RequestFlags::names(value.flags, value.flags_set);
        if (!names.empty()) emit(key, names);
      } else {
        decltype(auto) member = get_member(value, get<I>(reflect<T>::values));
        if constexpr (null_t<std::remove_cvref_t<decltype(member)>>) {
          if (!bool(member)) return;
        }
        emit(key, member);
      }
    });
    dump<'}'>(b, ix);
  }
};

template <>
struct to<JSON, luxir::api::SearchOp> : FlaggedRequestJsonWriter<luxir::api::SearchOp> {};
template <>
struct to<JSON, luxir::api::SearchRequest> : FlaggedRequestJsonWriter<luxir::api::SearchRequest> {};

// ----- Plugin arms: {"<name>": {...}} for a registered plugin query or op -----
// Every member of the object becomes a param, except "ops" when `ops` is given
// (a PluginOp's nested operations). The name and keys are copied: a key can
// live in a scratch buffer when it was escaped.
template <auto Opts, typename Ops>
void readPluginArm(std::string_view name, std::string_view &nameOut,
                   luxir::api::map_view<std::string_view, ::hpp_proto::indirect_view<luxir::api::Val>> &paramsOut,
                   Ops *ops, hpp_proto::concepts::is_non_owning_context auto &ctx,
                   auto &it, auto &end) {
  namespace api = luxir::api;
  auto copy = [&](std::string_view text) {
    char *stored = (char *)ctx.memory_resource().allocate(text.size(), 1);
    std::memcpy(stored, text.data(), text.size());
    return std::string_view(stored, text.size());
  };
  nameOut = copy(name);
  if constexpr (!check_ws_handled(Opts)) {
    if (skip_ws<Opts>(ctx, it, end)) return;
  }
  if ((char)*it != '{') {
    ctx.error = error_code::syntax_error;
    ctx.custom_error_message = "a plugin arm takes an object of parameters";
    return;
  }
  static constexpr auto O = ws_handled<Opts>();
  static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
  std::string_view key;
  decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
  decltype(auto) params = ::hpp_proto::detail::as_modifiable(ctx, paramsOut);
  util::scan_object_fields<O, true>(
      ctx, it, end, keyTarget, [](auto &, auto &) {},
      [&](auto &vit, auto &vend) {
        if constexpr (!std::is_same_v<Ops, std::nullptr_t>) {
          if (key == "ops") {
            decltype(auto) target = ::hpp_proto::detail::as_modifiable(ctx, *ops);
            glz::util::parse_repeated<V>(true, target, ctx, vit, vend);
            return bool(ctx.error);
          }
        }
        size_t index = params.size();
        for (size_t i = 0; i < index; i++) {
          if (params[i].first == key) {
            ctx.error = error_code::syntax_error;
            ctx.custom_error_message = "duplicate plugin parameter";
            return true;
          }
        }
        auto *val = new (ctx.memory_resource().allocate(sizeof(api::Val), alignof(api::Val))) api::Val();
        util::from_json<V>(*val, ctx, vit, vend);
        params.resize(index + 1);
        params[index] = {copy(key), ::hpp_proto::indirect_view<api::Val>(val)};
        return bool(ctx.error);
      },
      [](auto &, auto &) {});
}

// ----- SearchOp: canonical one-arm object plus optional domain, or ExprOp sugar -----
static_assert(std::variant_size_v<decltype(luxir::api::SearchOp::kind)> == 8,
              "SearchOp gained an arm: update its hand-written JSON arm dispatch and sugar");
template <>
struct from<JSON, luxir::api::SearchOp> {
  template <auto Opts>
  static void op(luxir::api::SearchOp &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) return;
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      auto &expr = value.kind.template emplace<api::ExprOp>();
      util::from_json<O>(expr.expr, ctx, it, end);
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    bool sawArm = false;
    bool sawDomain = false;
    bool sawFlags = false;
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "flags" && !sawFlags) {
            sawFlags = true;
            readRequestFlags<V>(value, ctx, vit, vend);
            return bool(ctx.error);
          }
          if (key == "domain" && !sawDomain) {
            sawDomain = true;
            util::from_json<V>(value.domain, ctx, vit, vend);
            return bool(ctx.error);
          }
          if (sawArm) {
            ctx.error = error_code::unknown_key;
            return true;
          }
          sawArm = true;
          auto arm = [&]<typename T>(std::in_place_type_t<T>) {
            util::from_json<V>(value.kind.template emplace<T>(), ctx, vit, vend);
          };
          if (key == "top_docs") {
            arm(std::in_place_type<api::TopDocs>);
          } else if (key == "fusion") {
            arm(std::in_place_type<api::Fusion>);
          } else if (key == "field_facet") {
            arm(std::in_place_type<api::FieldFacet>);
          } else if (key == "range_facet") {
            arm(std::in_place_type<api::RangeFacet>);
          } else if (key == "query_facet") {
            arm(std::in_place_type<api::QueryFacet>);
          } else if (key == "expr_op") {
            arm(std::in_place_type<api::ExprOp>);
          } else if (key == "plugin") {
            arm(std::in_place_type<api::PluginOp>);
          } else if (api::isPluginOpArm(key)) {
            auto &plugin = value.kind.template emplace<api::PluginOp>();
            readPluginArm<V>(key, plugin.name, plugin.params, &plugin.ops, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- WeightedField: canonical object, or name/name^weight string sugar -----
template <>
struct from<JSON, luxir::api::WeightedField> {
  template <auto Opts>
  static void op(luxir::api::WeightedField& value,
                 hpp_proto::concepts::is_non_owning_context auto& ctx, auto& it, auto& end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) return;
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      util::from_json<O>(value.field, ctx, it, end);
      // A root string may finish with the successful end_reached marker.
      if (bool(ctx.error) && ctx.error != error_code::end_reached) return;
      if (!luxir::api::splitWeightedField(value.field, value.field, value.weight)) {
        ctx.error = error_code::syntax_error;
      }
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto&, auto&) {},
        [&](auto& vit, auto& vend) {
          if (key == "field") {
            util::from_json<V>(value.field, ctx, vit, vend);
          } else if (key == "weight") {
            util::from_json<V>(value.weight, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto&, auto&) {});
  }
};

// ----- SortSpec: a bare string is one clause ("expr", "expr asc", "expr desc");
// in the object form `field` is a bare-column alias for `expr` -----
template <>
struct from<JSON, luxir::api::SortSpec> {
  template <auto Opts>
  static void op(luxir::api::SortSpec &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      util::from_json<O>(value.expr, ctx, it, end);
      if (bool(ctx.error)) return;
      std::string_view expr;
      std::optional<std::string_view> dir;
      if (!luxir::api::splitSortClause(value.expr, expr, dir)) {
        ctx.error = error_code::syntax_error;
        return;
      }
      value.expr = expr;
      if (dir) {
        value.dir = *dir == "asc" ? luxir::api::SortSpec::SortDir::ASC
                                  : luxir::api::SortSpec::SortDir::DESC;
      }
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "expr" || key == "field") {
            util::from_json<V>(value.expr, ctx, vit, vend);
          } else if (key == "vars") {
            decltype(auto) vars = ::hpp_proto::detail::as_modifiable(ctx, value.vars);
            glz::util::parse_repeated<V>(true, vars, ctx, vit, vend);
          } else if (key == "dir") {
            util::from_json<V>(value.dir, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- sort lists: one clause (string or object) or a list of them -----
template <auto Opts>
void readSortList(auto &sort, auto &ctx, auto &it, auto &end) {
  if constexpr (!check_ws_handled(Opts)) {
    if (skip_ws<Opts>(ctx, it, end)) {
      return;
    }
  }
  static constexpr auto O = ws_handled<Opts>();
  if ((char)*it == '[') {
    glz::util::parse_repeated<O>(false, sort, ctx, it, end);
    return;
  }
  const std::size_t n = sort.size();
  sort.resize(n + 1);
  util::from_json<O>(sort[n], ctx, it, end);
}

template <>
struct from<JSON, luxir::api::FieldVariants> {
  template <auto Opts>
  static void op(luxir::api::FieldVariants &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    decltype(auto) entries = ::hpp_proto::detail::as_modifiable(ctx, value.entries);
    glz::util::parse_repeated<Opts>(true, entries, ctx, it, end);
  }
};
template <>
struct to<JSON, luxir::api::FieldVariants> {
  template <auto Opts, class... Args>
  static void op(const luxir::api::FieldVariants &value, Args &&...args) noexcept {
    serialize<JSON>::template op<Opts>(value.entries, std::forward<Args>(args)...);
  }
};
template <>
struct from<JSON, luxir::api::NormalizerDef> {
  template <auto Opts>
  static void op(luxir::api::NormalizerDef &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    decltype(auto) filters = ::hpp_proto::detail::as_modifiable(ctx, value.filters);
    glz::util::parse_repeated<Opts>(false, filters, ctx, it, end);
  }
};
template <>
struct to<JSON, luxir::api::NormalizerDef> {
  template <auto Opts, class... Args>
  static void op(const luxir::api::NormalizerDef &value, Args &&...args) noexcept {
    serialize<JSON>::template op<Opts>(value.filters, std::forward<Args>(args)...);
  }
};

// FieldDef.multi uses booleans for single/default and strings for explicit modes.
template <>
struct from<JSON, luxir::api::FieldDef::MultiMode> {
  template <auto Opts>
  static void op(luxir::api::FieldDef::MultiMode& value,
                 hpp_proto::concepts::is_non_owning_context auto& ctx, auto& it, auto& end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) return;
    }
    static constexpr auto O = ws_handled<Opts>();
    using Mode = luxir::api::FieldDef::MultiMode;
    if (*it == 't' || *it == 'f') {
      bool enabled = false;
      util::from_json<O>(enabled, ctx, it, end);
      if (ctx.error == error_code::none) value = enabled ? Mode::DEFAULT : Mode::SINGLE;
    } else if (*it == '"') {
      std::string_view mode;
      util::from_json<O>(mode, ctx, it, end);
      if (ctx.error != error_code::none) return;
      if (mode == "set") value = Mode::SET;
      else if (mode == "array") value = Mode::ARRAY;
      else ctx.error = error_code::syntax_error;
    } else {
      ctx.error = error_code::syntax_error;
    }
  }
};
template <>
struct to<JSON, luxir::api::FieldDef::MultiMode> {
  template <auto Opts, class... Args>
  static void op(luxir::api::FieldDef::MultiMode value, Args&&... args) noexcept {
    using Mode = luxir::api::FieldDef::MultiMode;
    if (value == Mode::SINGLE || value == Mode::DEFAULT) {
      serialize<JSON>::template op<Opts>(value == Mode::DEFAULT, std::forward<Args>(args)...);
    } else {
      serialize<JSON>::template op<Opts>(std::string_view(value == Mode::SET ? "set" : "array"),
                                         std::forward<Args>(args)...);
    }
  }
};

// ----- FieldDef: canonical object, or a bare string (type-only sugar) -----
// A type string that is not a built-in type names a plugin field type:
// {"type": "pair", "params": {...}} == {"plugin_type": {"name": "pair", "params": {...}}}.
template <>
struct from<JSON, luxir::api::FieldDef> {
  // The built-in type names (FieldClass JSON names).
  static std::optional<luxir::api::FieldDef::FieldClass> builtinType(std::string_view name) {
    using C = luxir::api::FieldDef::FieldClass;
    static constexpr std::pair<std::string_view, C> names[] = {
        {"string", C::STRING}, {"text", C::TEXT}, {"int", C::INT}, {"float", C::FLOAT}, {"double", C::DOUBLE},
        {"id", C::ID}, {"vector", C::VECTOR}, {"date", C::DATE}, {"geo_point", C::GEO_POINT},
        {"boolean", C::BOOLEAN}, {"ip", C::IP}, {"decimal", C::DECIMAL}};
    for (const auto& [n, c] : names) {
      if (n == name) return c;
    }
    return std::nullopt;
  }

  // A type value: a built-in name or an enum number into type, any other
  // [a-z0-9_]+ name into plugin_type. Returns whether it named a plugin type.
  template <auto Opts>
  static bool readType(luxir::api::FieldDef &value, auto &ctx, auto &it, auto &end) {
    if ((char)*it != '"') {
      util::from_json<Opts>(value.type, ctx, it, end);
      return false;
    }
    std::string_view name;
    util::from_json<Opts>(name, ctx, it, end);
    if (ctx.error != error_code::none) return false;
    bool pluginName = !name.empty() && std::ranges::all_of(name, [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
    if (auto builtin = builtinType(name)) {
      value.type = *builtin;
    } else if (pluginName) {
      value.plugin_type.emplace().name = name;
      return true;
    } else {
      ctx.error = error_code::unexpected_enum;
    }
    return false;
  }

  template <auto Opts>
  static void op(luxir::api::FieldDef &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      readType<O>(value, ctx, it, end);
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    // "type": "<plugin name>" and "params" may come in either order, and
    // neither combines with "plugin_type".
    bool pluginTypeKey = false, paramsKey = false;
    bool typedPlugin = false;  // "type" named a plugin type
    decltype(luxir::api::AnalyzerComponent::params) params;
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "parent") {
            util::from_json<V>(value.parent, ctx, vit, vend);
          } else if (key == "type") {
            typedPlugin = readType<V>(value, ctx, vit, vend) || typedPlugin;
          } else if (key == "plugin_type") {
            pluginTypeKey = true;
            util::from_json<V>(value.plugin_type, ctx, vit, vend);
          } else if (key == "params") {
            paramsKey = true;
            decltype(auto) target = ::hpp_proto::detail::as_modifiable(ctx, params);
            glz::util::parse_repeated<V>(true, target, ctx, vit, vend);
          } else if (key == "index") {
            util::from_json<V>(value.index, ctx, vit, vend);
          } else if (key == "long_terms") {
            util::from_json<V>(value.long_terms, ctx, vit, vend);
          } else if (key == "unit") {
            util::from_json<V>(value.unit, ctx, vit, vend);
          } else if (key == "column") {
            util::from_json<V>(value.column, ctx, vit, vend);
          } else if (key == "multi") {
            // The generic enum utility treats every non-string as an integer.
            from<JSON, luxir::api::FieldDef::MultiMode>::template op<V>(
                value.multi.emplace(), ctx, vit, vend);
          } else if (key == "analyzer") {
            util::from_json<V>(value.analyzer, ctx, vit, vend);
          } else if (key == "variants") {
            util::from_json<V>(value.variants, ctx, vit, vend);
          } else if (key == "defaults") {
            util::from_json<V>(value.defaults, ctx, vit, vend);
          } else if (key == "normalizer") {
            util::from_json<V>(value.normalizer, ctx, vit, vend);
          } else if (key == "collation") {
            util::from_json<V>(value.collation, ctx, vit, vend);
          } else if (key == "stored") {
            util::from_json<V>(value.stored, ctx, vit, vend);
          } else if (key == "stored_resource") {
            util::from_json<V>(value.stored_resource, ctx, vit, vend);
          } else if (key == "dims") {
            util::from_json<V>(value.dims, ctx, vit, vend);
          } else if (key == "truncate_dims") {
            util::from_json<V>(value.truncate_dims, ctx, vit, vend);
          } else if (key == "metric") {
            util::from_json<V>(value.metric, ctx, vit, vend);
          } else if (key == "normalized") {
            util::from_json<V>(value.normalized, ctx, vit, vend);
          } else if (key == "normalize_on_write") {
            util::from_json<V>(value.normalize_on_write, ctx, vit, vend);
          } else if (key == "ann") {
            util::from_json<V>(value.ann, ctx, vit, vend);
          } else if (key == "scale") {
            util::from_json<V>(value.scale, ctx, vit, vend);
          } else if (key == "ignore_malformed") {
            util::from_json<V>(value.ignore_malformed, ctx, vit, vend);
          } else if (key == "ignore_z_value") {
            util::from_json<V>(value.ignore_z_value, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
    if (ctx.error != error_code::none) return;
    // "params" belongs to a plugin type named by "type"; "plugin_type" carries its own.
    if (typedPlugin && pluginTypeKey) {
      ctx.error = error_code::syntax_error;
      ctx.custom_error_message = "a field takes type or plugin_type, not both";
      return;
    }
    if (paramsKey && !typedPlugin) {
      ctx.error = error_code::syntax_error;
      ctx.custom_error_message = "params belongs to a plugin field type named by type";
      return;
    }
    if (paramsKey) value.plugin_type->params = params;
  }
};

// ----- AnalyzerComponent: canonical object, or a bare string (name-only sugar) -----
template <>
struct from<JSON, luxir::api::AnalyzerComponent> {
  template <auto Opts>
  static void op(luxir::api::AnalyzerComponent &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      util::from_json<O>(value.name, ctx, it, end);
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "name") {
            util::from_json<V>(value.name, ctx, vit, vend);
          } else if (key == "params") {
            decltype(auto) params = ::hpp_proto::detail::as_modifiable(ctx, value.params);
            glz::util::parse_repeated<V>(true, params, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- AnalyzerDef: canonical object, or a bare string (named-analyzer sugar) -----
// Reads: "english" == {"name": "english"}. Writes stay the generated canonical form.
template <>
struct from<JSON, luxir::api::AnalyzerDef> {
  template <auto Opts>
  static void op(luxir::api::AnalyzerDef &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      util::from_json<O>(value.name, ctx, it, end);
      if (!bool(ctx.error) && value.name.empty()) {
        ctx.error = error_code::syntax_error;
        ctx.custom_error_message = "analyzer name must not be empty; use {} to inherit or name an analyzer";
      }
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "tokenizer") {
            util::from_json<V>(value.tokenizer, ctx, vit, vend);
          } else if (key == "filters") {
            decltype(auto) filters = ::hpp_proto::detail::as_modifiable(ctx, value.filters);
            glz::util::parse_repeated<V>(false, filters, ctx, vit, vend);
          } else if (key == "char_filters") {
            decltype(auto) charFilters = ::hpp_proto::detail::as_modifiable(ctx, value.char_filters);
            glz::util::parse_repeated<V>(false, charFilters, ctx, vit, vend);
          } else if (key == "name") {
            util::from_json<V>(value.name, ctx, vit, vend);
            if (!bool(ctx.error) && value.name.empty()) {
              ctx.error = error_code::syntax_error;
              ctx.custom_error_message = "analyzer name must not be empty; use {} to inherit or name an analyzer";
            }
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// Writes: the bare name for a parameterless component, else {"name", "params"}.
template <>
struct to<JSON, luxir::api::AnalyzerComponent> {
  template <auto Opts, class B>
  static void op(const luxir::api::AnalyzerComponent &value, is_context auto &ctx, B &b,
                 auto &ix) noexcept {
    if (value.params.empty()) {
      serialize<JSON>::template op<Opts>(value.name, ctx, b, ix);
      return;
    }
    dump<"{\"name\":">(b, ix);
    serialize<JSON>::template op<Opts>(value.name, ctx, b, ix);
    dump<",\"params\":">(b, ix);
    serialize<JSON>::template op<Opts>(value.params, ctx, b, ix);
    dump<'}'>(b, ix);
  }
};

// ----- Query: canonical one-arm object, or a bare string (expr sugar) -----
// The hand-written key dispatch replaces the generated oneof-object read so the
// string form can be recognized first; keys and arms must track the Query oneof
// (the static_assert in query/ExprFunctions.h ARM_NAMES pins the same list).
template <>
struct from<JSON, luxir::api::Query> {
  template <auto Opts>
  static void op(luxir::api::Query &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      // bare string in query position = an expr expression
      auto &e = value.kind.template emplace<api::ExprQuery>();
      util::from_json<O>(e.q, ctx, it, end);
      return;
    }
    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    bool sawArm = false;
    bool sawSiblingBoost = false;
    std::optional<float> siblingBoost;
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          // "boost" is type-directed: a JSON object is the oneof arm, while a
          // number is the sole non-arm sibling accepted on a Query object.
          if (key == "boost") {
            auto probe = vit;
            while (probe != vend) {
              char c = (char)*probe;
              if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
              ++probe;
            }
            if (probe == vend || (char)*probe != '{') {
              if (sawSiblingBoost) {
                ctx.error = error_code::unknown_key;
                return true;
              }
              sawSiblingBoost = true;
              util::from_json<V>(siblingBoost.emplace(), ctx, vit, vend);
              auto after = vit;
              while (after != vend) {
                char c = (char)*after;
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
                ++after;
              }
              if (!bool(ctx.error) && !sawArm && after != vend && (char)*after == '}') {
                ctx.error = error_code::unknown_key;
              }
              return bool(ctx.error);
            }
          }
          // exactly one arm: a Query object IS the oneof, so a second key is
          // an error (proto3 canonical JSON), not a silent last-wins
          if (sawArm) {
            ctx.error = error_code::unknown_key;
            return true;
          }
          sawArm = true;
          auto arm = [&]<typename T>(std::in_place_type_t<T>) {
            util::from_json<V>(value.kind.template emplace<T>(), ctx, vit, vend);
          };
          if (key == "match") {
            arm(std::in_place_type<api::Match>);
          } else if (key == "boolean") {
            arm(std::in_place_type<api::BooleanQuery>);
          } else if (key == "all") {
            util::from_json<V>(value.kind.template emplace<bool>(), ctx, vit, vend);
          } else if (key == "exists") {
            arm(std::in_place_type<api::ExistsQuery>);
          } else if (key == "phrase") {
            arm(std::in_place_type<api::PhraseQuery>);
          } else if (key == "knn") {
            arm(std::in_place_type<api::KnnQuery>);
          } else if (key == "constant_score") {
            arm(std::in_place_type<api::ConstantScoreQuery>);
          } else if (key == "prefix") {
            arm(std::in_place_type<api::PrefixQuery>);
          } else if (key == "wildcard") {
            arm(std::in_place_type<api::WildcardQuery>);
          } else if (key == "regex") {
            arm(std::in_place_type<api::RegexQuery>);
          } else if (key == "any_of") {
            arm(std::in_place_type<api::AnyOfQuery>);
          } else if (key == "fuzzy") {
            arm(std::in_place_type<api::FuzzyQuery>);
          } else if (key == "simple_query") {
            arm(std::in_place_type<api::SimpleQuery>);
          } else if (key == "range") {
            arm(std::in_place_type<api::RangeQuery>);
          } else if (key == "geo_box") {
            arm(std::in_place_type<api::GeoBoxQuery>);
          } else if (key == "geo_distance") {
            arm(std::in_place_type<api::GeoDistanceQuery>);
          } else if (key == "boost") {
            arm(std::in_place_type<api::BoostQuery>);
          } else if (key == "rescore") {
            arm(std::in_place_type<api::RescoreQuery>);
          } else if (key == "expr") {
            arm(std::in_place_type<api::ExprQuery>);
          } else if (key == "multi_match") {
            arm(std::in_place_type<api::MultiMatchQuery>);
          } else if (key == "dis_max") {
            arm(std::in_place_type<api::DisMaxQuery>);
          } else if (key == "plugin") {
            arm(std::in_place_type<api::PluginQuery>);
          } else if (api::isPluginQueryArm(key)) {
            auto &plugin = value.kind.template emplace<api::PluginQuery>();
            readPluginArm<V>(key, plugin.name, plugin.params, (std::nullptr_t *)nullptr, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [&](auto &after, auto &) {
          if ((char)*after != '}' || bool(ctx.error) || !sawSiblingBoost || !sawArm) return;
          auto &mr = ctx.memory_resource();
          auto *child = (api::Query *)mr.allocate(sizeof(api::Query), alignof(api::Query));
          new (child) api::Query(value);
          api::BoostQuery wrapper;
          wrapper.query = child;
          wrapper.boost = siblingBoost;
          value.kind = wrapper;
        });
  }
};

inline void prefixFilterJsonError(auto &ctx, size_t index) {
  std::string detail(ctx.custom_error_message);
  std::string message = "filter[" + std::to_string(index) + "]";
  if (!detail.empty()) {
    message += ": ";
    message += detail;
  }
  char* stored = (char*)ctx.memory_resource().allocate(message.size(), 1);
  std::memcpy(stored, message.data(), message.size());
  ctx.custom_error_message = std::string_view(stored, message.size());
}

template <auto Options>
void readFilterArray(std::span<const luxir::api::Filter> &value,
                     hpp_proto::concepts::is_non_owning_context auto &ctx,
                     auto &it, auto &end) {
  constexpr auto Opts = ws_handled_off<Options>();
  if (!util::parse_opening<Options>('[', ctx, it, end)) return;
  if (skip_ws<Opts>(ctx, it, end)) return;
  const auto n = util::number_of_elements<Opts>(']', ctx, it, end);
  if (bool(ctx.error)) return;

  decltype(auto) filters = ::hpp_proto::detail::as_modifiable(ctx, value);
  const size_t oldSize = filters.size();
  filters.resize(oldSize + n);
  for (size_t i = oldSize; i < filters.size(); i++) {
    util::from_json<Opts>(filters[i], ctx, it, end);
    if (bool(ctx.error)) {
      prefixFilterJsonError(ctx, i);
      return;
    }
    if (skip_ws<Opts>(ctx, it, end)) return;
    if (i + 1 < filters.size()
        && match_invalid_end<',', Opts>(ctx, it, end)) {
      return;
    }
  }
  util::match_ending<Opts>(']', ctx, it, end);
}

// ----- Filter: a bare Query, or a strict routing wrapper -----
template <>
struct from<JSON, luxir::api::Filter> {
  template <auto Opts>
  static void op(luxir::api::Filter &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) return;
    }
    static constexpr auto O = ws_handled<Opts>();
    if ((char)*it == '"') {
      from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
          value.query, ctx, it, end);
      return;
    }
    if ((char)*it != '{') {
      from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
          value.query, ctx, it, end);
      return;
    }

    // A valid wrapper's first key must be query or except_ops. Any other
    // object is parsed wholly as a structured Query.
    auto probe = it;
    ++probe;
    static constexpr auto P = ws_handled_off<Opts>();
    if (skip_ws<P>(ctx, probe, end)) return;
    bool wrapper = false;
    if ((char)*probe != '}') {
      std::string_view firstKey;
      decltype(auto) firstKeyTarget =
          ::hpp_proto::detail::as_modifiable(ctx, firstKey);
      util::parse_key_and_colon<ws_handled<Opts>()>(
          firstKeyTarget, ctx, probe, end);
      if (bool(ctx.error)) return;
      wrapper = firstKey == "query" || firstKey == "except_ops";
    }
    if (!wrapper) {
      from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
          value.query, ctx, it, end);
      return;
    }

    static constexpr auto V = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    bool sawQuery = false;
    bool sawExceptOps = false;
    util::scan_object_fields<O, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "query" && !sawQuery) {
            sawQuery = true;
            from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<V>(
                value.query, ctx, vit, vend);
          } else if (key == "except_ops" && !sawExceptOps) {
            sawExceptOps = true;
            decltype(auto) exceptOps =
                ::hpp_proto::detail::as_modifiable(ctx, value.except_ops);
            glz::util::parse_repeated<V>(false, exceptOps, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [&](auto &after, auto &) {
          if ((char)*after != '}' || bool(ctx.error)) return;
          if (!sawQuery || !sawExceptOps) {
            ctx.error = error_code::missing_key;
            ctx.custom_error_message =
                "routing wrapper requires exactly query and except_ops";
          } else if (value.except_ops.empty()) {
            ctx.error = error_code::syntax_error;
            ctx.custom_error_message =
                "except_ops must be nonempty; use the bare filter form instead";
          }
        });
  }
};

template <>
struct to<JSON, luxir::api::Filter> {
  template <auto Opts, class B>
  static void op(const luxir::api::Filter &value, is_context auto &ctx,
                 B &b, auto &ix) noexcept {
    if (value.except_ops.empty()) {
      if (value.query.has_value()) {
        serialize<JSON>::template op<Opts>(*value.query, ctx, b, ix);
      } else {
        dump<"{}">(b, ix);
      }
      return;
    }
    dump<"{\"query\":" >(b, ix);
    if (value.query.has_value()) {
      serialize<JSON>::template op<Opts>(*value.query, ctx, b, ix);
    } else {
      dump<"null">(b, ix);
    }
    dump<",\"except_ops\":" >(b, ix);
    serialize<JSON>::template op<Opts>(value.except_ops, ctx, b, ix);
    dump<'}'>(b, ix);
  }
};

template <>
struct from<JSON, luxir::api::Collapse> {
  template <auto Opts>
  static void op(luxir::api::Collapse &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) return;
    }
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    auto readFields = [&](auto &vit, auto &vend) {
      value.field = {};
      decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, value.field);
      if (skip_ws<O>(ctx, vit, vend)) return;
      if ((char)*vit == '[') glz::util::parse_repeated<O>(false, fields, ctx, vit, vend);
      else {
        fields.resize(1);
        util::from_json<O>(fields[0], ctx, vit, vend);
      }
    };
    if ((char)*it == '"') {
      value.missing = luxir::api::Collapse_::MissingPolicy::EXPAND;
      value.head_sort = {};
      value.sort = {};
      value.get_count = false;
      value.ops = {};
      readFields(it, end);
      return;
    }
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "field") readFields(vit, vend);
          else if (key == "missing") util::from_json<O>(value.missing, ctx, vit, vend);
          else if (key == "get_count") util::from_json<O>(value.get_count, ctx, vit, vend);
          else if (key == "ops") {
            decltype(auto) ops = ::hpp_proto::detail::as_modifiable(ctx, value.ops);
            glz::util::parse_repeated<O>(true, ops, ctx, vit, vend);
          }
          else if (key == "head_sort" || key == "sort") {
            decltype(auto) sort = ::hpp_proto::detail::as_modifiable(
                ctx, key == "head_sort" ? value.head_sort : value.sort);
            readSortList<O>(sort, ctx, vit, vend);
          }
          else { ctx.error = error_code::unknown_key; return true; }
          return bool(ctx.error);
        }, [](auto &, auto &) {});
  }
};

template <>
struct from<JSON, luxir::api::Diversify> {
  template <auto Opts>
  static void op(luxir::api::Diversify &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    auto readFields = [&](auto &vit, auto &vend) {
      value.field = {};
      decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, value.field);
      if (skip_ws<O>(ctx, vit, vend)) return;
      if ((char)*vit == '[') glz::util::parse_repeated<O>(false, fields, ctx, vit, vend);
      else {
        fields.resize(1);
        util::from_json<O>(fields[0], ctx, vit, vend);
      }
    };
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "field") readFields(vit, vend);
          else if (key == "missing") util::from_json<O>(value.missing, ctx, vit, vend);
          else if (key == "max_per_key") util::from_json<O>(value.max_per_key, ctx, vit, vend);
          else { ctx.error = error_code::unknown_key; return true; }
          return bool(ctx.error);
        }, [](auto &, auto &) {});
  }
};

template <>
struct from<JSON, luxir::api::TopDocs> {
  template <auto Opts>
  static void op(luxir::api::TopDocs &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "query") {
            from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
                value.query, ctx, vit, vend);
          } else if (key == "filter") {
            readFilterArray<O>(value.filter, ctx, vit, vend);
          } else if (key == "offset") {
            util::from_json<O>(value.offset, ctx, vit, vend);
          } else if (key == "limit") {
            util::from_json<O>(value.limit, ctx, vit, vend);
          } else if (key == "count_limit") {
            util::from_json<O>(value.count_limit, ctx, vit, vend);
          } else if (key == "get_scores") {
            util::from_json<O>(value.get_scores, ctx, vit, vend);
          } else if (key == "min_score") {
            util::from_json<O>(value.min_score, ctx, vit, vend);
          } else if (key == "fields") {
            decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, value.fields);
            glz::util::parse_repeated<O>(false, fields, ctx, vit, vend);
          } else if (key == "sort") {
            decltype(auto) sort = ::hpp_proto::detail::as_modifiable(ctx, value.sort);
            readSortList<O>(sort, ctx, vit, vend);
          } else if (key == "diversify") {
            util::from_json<O>(value.diversify, ctx, vit, vend);
          } else if (key == "collapse") {
            util::from_json<O>(value.collapse, ctx, vit, vend);
          } else if (key == "batch_size") {
            util::from_json<O>(value.batch_size, ctx, vit, vend);
          } else if (key == "document_format") {
            util::from_json<O>(value.document_format, ctx, vit, vend);
          } else if (key == "ops") {
            decltype(auto) ops = ::hpp_proto::detail::as_modifiable(ctx, value.ops);
            glz::util::parse_repeated<O>(true, ops, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

template <>
struct from<JSON, luxir::api::Fusion> {
  template <auto Opts>
  static void op(luxir::api::Fusion &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          if (key == "sources") {
            decltype(auto) sources =
                ::hpp_proto::detail::as_modifiable(ctx, value.sources);
            glz::util::parse_repeated<O>(true, sources, ctx, vit, vend);
          } else if (key == "filter") {
            readFilterArray<O>(value.filter, ctx, vit, vend);
          } else if (key == "limit") {
            util::from_json<O>(value.limit, ctx, vit, vend);
          } else if (key == "offset") {
            util::from_json<O>(value.offset, ctx, vit, vend);
          } else if (key == "count_limit") {
            util::from_json<O>(value.count_limit, ctx, vit, vend);
          } else if (key == "get_scores") {
            util::from_json<O>(value.get_scores, ctx, vit, vend);
          } else if (key == "fields") {
            decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, value.fields);
            glz::util::parse_repeated<O>(false, fields, ctx, vit, vend);
          } else if (key == "batch_size") {
            util::from_json<O>(value.batch_size, ctx, vit, vend);
          } else if (key == "document_format") {
            util::from_json<O>(value.document_format, ctx, vit, vend);
          } else if (key == "ops") {
            decltype(auto) ops = ::hpp_proto::detail::as_modifiable(ctx, value.ops);
            glz::util::parse_repeated<O>(true, ops, ctx, vit, vend);
          } else if (key == "weights") {
            decltype(auto) weights = ::hpp_proto::detail::as_modifiable(ctx, value.weights);
            glz::util::parse_repeated<O>(true, weights, ctx, vit, vend);
          } else if (key == "collapse") {
            util::from_json<O>(value.collapse, ctx, vit, vend);
          } else if (key == "rrf" || key == "linear") {
            // one method arm: a second one is an error (proto3 canonical
            // JSON), not a silent last-wins
            if (value.method.index() != 0) {
              ctx.error = error_code::syntax_error;
              ctx.custom_error_message = "fusion takes exactly one method: rrf or linear";
              return true;
            }
            if (key == "rrf") {
              util::from_json<O>(value.method.template emplace<luxir::api::RrfFusion>(),
                                 ctx, vit, vend);
            } else {
              util::from_json<O>(value.method.template emplace<luxir::api::LinearFusion>(),
                                 ctx, vit, vend);
            }
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
  }
};

// ----- Val: a raw JSON value (untagged) -----
template <>
struct from<JSON, luxir::api::Val> {
  template <auto Opts>
  static void op(luxir::api::Val &value, hpp_proto::concepts::is_non_owning_context auto &ctx,
                 auto &it, auto &end) {
    namespace api = luxir::api;
    if constexpr (!check_ws_handled(Opts)) {
      if (skip_ws<Opts>(ctx, it, end)) {
        return;
      }
    }
    if (++ctx.depth > max_recursive_depth_limit) [[unlikely]] {
      ctx.error = error_code::exceeded_max_recursive_depth;
      return;
    }
    constexpr auto O = ws_handled<Opts>();
    switch ((char)*it) {
      case 'n': {
        auto &arm = value.kind.template emplace<google::protobuf::NullValue>();
        from<JSON, google::protobuf::NullValue>::template op<O>(arm, ctx, it, end);
        break;
      }
      case '"':
        util::from_json<O>(value.kind.template emplace<std::string_view>(), ctx, it, end);
        break;
      case 't':
      case 'f':
        util::from_json<O>(value.kind.template emplace<bool>(), ctx, it, end);
        break;
      case '{': {
        auto &m = value.kind.template emplace<api::Map>();
        from<JSON, api::Map>::template op<O>(m, ctx, it, end);
        break;
      }
      case '[': {
        if (api::jsond::readNumberArray<O>(value, ctx, it, end)) break;
        api::ArrVal tmp{};
        {
          decltype(auto) elems = ::hpp_proto::detail::as_modifiable(ctx, tmp.v);
          glz::util::parse_repeated<O>(false, elems, ctx, it, end);
        }
        if (bool(ctx.error)) [[unlikely]] {
          break;
        }
        // All-number arrays were taken by readNumberArray; an all-string array
        // collapses to ArrStr, anything else stays a generic ArrVal.
        bool allS = !tmp.v.empty();
        for (const auto &e : tmp.v) allS = allS && std::holds_alternative<std::string_view>(e.kind);
        if (allS) {
          size_t n = tmp.v.size();
          auto *a = (std::string_view *)ctx.memory_resource().allocate(
              n * sizeof(std::string_view), alignof(std::string_view));
          for (size_t i = 0; i < n; i++) a[i] = std::get<std::string_view>(tmp.v[i].kind);
          value.kind.template emplace<api::ArrStr>().v = {a, n};
        } else {
          value.kind.template emplace<api::ArrVal>() = tmp;
        }
        break;
      }
      default: {
        api::jsond::Number num;
        if (!api::jsond::parseNumber<O.null_terminated>(it, end, num)) [[unlikely]] {
          ctx.error = error_code::parse_number_failure;
          break;
        }
        if (num.isInt) value.kind.template emplace<std::int64_t>(num.i);
        else value.kind.template emplace<double>(num.d);
        it = num.next;
        break;
      }
    }
    --ctx.depth;
  }
};

template <>
struct to<JSON, luxir::api::Val> {
  static constexpr bool can_error = true; // Val->ArrVal->Val recursion: don't derive constexpr
  template <auto Opts>
  // NOLINTNEXTLINE(misc-no-recursion)
  static void op(const luxir::api::Val &value, is_context auto &ctx, auto &b, auto &ix) {
    namespace api = luxir::api;
    auto emit = [&](const auto &v) { serialize<JSON>::template op<Opts>(v, ctx, b, ix); };
    std::visit(api::jsond::overloaded{
                   [&](std::monostate) { dump<not check_write_unchecked(Opts)>("null", b, ix); },
                   [&](google::protobuf::NullValue) {
                     dump<not check_write_unchecked(Opts)>("null", b, ix);
                   },
                   [&](std::string_view v) { emit(v); },
                   [&](std::int64_t v) { emit(v); },
                   [&](double v) { emit(v); },
                   [&](float v) { emit(v); },
                   [&](bool v) { emit(v); },
                   [&](::hpp_proto::bytes_view v) { emit(v); }, // base64 string
                   [&](const api::Map &v) { emit(v); },
                   [&](const api::ArrVal &v) { emit(v.v); },
                   [&](const api::ArrStr &v) { emit(v.v); },
                   [&](const api::ArrInt &v) { emit(v.v); },
                   [&](const api::ArrFloat &v) { emit(v.v); },
                   [&](const api::ArrDouble &v) { emit(v.v); },
                   [&](const api::ArrBin &v) { emit(v.v); },
                   [&](const api::Vector &v) { emit(v); },
                   [&](const api::ArrVector &v) { emit(v.v); },
                   // Result-only arms: emitted via their generated metadata (a JSON
                   // object); reads never produce them (they map back to Map).
                   [&](const api::DocList &v) { emit(v); },
                   [&](const api::FacetResult &v) { emit(v); },
               },
               value.kind);
  }
};

// Commit counts accept either a JSON integer or a string (including "all").
// ----- SearchRequest: full form, root top_docs shorthand, or both at one root -----
// Read accepts request-level keys (request_id, collection, ops, freshness_ms,
// time_zone, response_format, profile, max_parallel) and TopDocs keys mixed at
// the root: any TopDocs key lazily creates the shorthand op, registered as
// ops["q"]. "ops" is ambiguous until the scan ends - it is SearchRequest.ops in
// the full form but the shorthand op's SUB-ops once any TopDocs key appears -
// so it parses into value.ops and moves to the shorthand op at the end (both
// sides are the same map type). Unknown keys error in one strict pass; writes
// stay canonical (echo mode always shows the full form).
//
// The two key lists hand-mirror the message definitions; a field added to
// SearchRequest or TopDocs in the proto must be added here or shorthand
// requests carrying it fail with unknown_key (strict, so the gap is loud).
template <>
struct from<JSON, luxir::api::SearchRequest> {
  template <auto Opts>
  static void op(luxir::api::SearchRequest &value,
                 hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    namespace api = luxir::api;
    static constexpr auto O = opening_handled_off<ws_handled_off<Opts>()>();
    api::SearchOp *shorthandOp = nullptr;
    api::TopDocs *td = nullptr;
    bool reusedShorthand = false;
    bool sawFlags = false;
    // A second read can overlay shorthand fields onto an already-decoded request.
    // Capture q before scanning so an ordinary shorthand body's root "ops" key
    // remains sub-ops even when that map itself contains an op named q.
    if (const auto *q = value.ops.find("q")) {
      auto *op = const_cast<api::SearchOp *>(q->pointer());
      if (op != nullptr) {
        td = std::get_if<api::TopDocs>(&op->kind);
        reusedShorthand = td != nullptr;
      }
    }
    auto shorthand = [&]() -> api::TopDocs & {
      if (td == nullptr) {
        void *addr = ctx.memory_resource().allocate(sizeof(api::SearchOp), alignof(api::SearchOp));
        shorthandOp = new (addr) api::SearchOp();
        td = &shorthandOp->kind.template emplace<api::TopDocs>();
      }
      return *td;
    };
    std::string_view key;
    decltype(auto) keyTarget = ::hpp_proto::detail::as_modifiable(ctx, key);
    util::scan_object_fields<Opts, true>(
        ctx, it, end, keyTarget, [](auto &, auto &) {},
        [&](auto &vit, auto &vend) {
          // request-level keys
          if (key == "request_id") {
            util::from_json<O>(value.request_id, ctx, vit, vend);
          } else if (key == "collection") {
            util::from_json<O>(value.collection, ctx, vit, vend);
          } else if (key == "ops") {
            decltype(auto) ops = ::hpp_proto::detail::as_modifiable(ctx, value.ops);
            glz::util::parse_repeated<O>(true, ops, ctx, vit, vend);
          } else if (key == "flags" && !sawFlags) {
            sawFlags = true;
            readRequestFlags<O>(value, ctx, vit, vend);
          } else if (key == "freshness_ms") {
            util::from_json<O>(value.freshness_ms, ctx, vit, vend);
          } else if (key == "min_commit") {
            util::from_json<O>(value.min_commit, ctx, vit, vend);
          } else if (key == "min_commit_timeout_ms") {
            util::from_json<O>(value.min_commit_timeout_ms, ctx, vit, vend);
          } else if (key == "time_zone") {
            util::from_json<O>(value.time_zone, ctx, vit, vend);
          } else if (key == "response_format") {
            util::from_json<O>(value.response_format, ctx, vit, vend);
          } else if (key == "profile") {
            util::from_json<O>(value.profile, ctx, vit, vend);
          } else if (key == "max_parallel") {
            util::from_json<O>(value.max_parallel, ctx, vit, vend);
          // shorthand top_docs keys
          } else if (key == "query") {
            from<JSON, ::hpp_proto::optional_indirect_view<api::Query>>::template op<O>(
                shorthand().query, ctx, vit, vend);
          } else if (key == "filter") {
            readFilterArray<O>(shorthand().filter, ctx, vit, vend);
          } else if (key == "offset") {
            util::from_json<O>(shorthand().offset, ctx, vit, vend);
          } else if (key == "limit") {
            util::from_json<O>(shorthand().limit, ctx, vit, vend);
          } else if (key == "fields") {
            auto &topDocs = shorthand();
            if (reusedShorthand) topDocs.fields = {};
            decltype(auto) fields = ::hpp_proto::detail::as_modifiable(ctx, topDocs.fields);
            glz::util::parse_repeated<O>(false, fields, ctx, vit, vend);
          } else if (key == "sort") {
            auto &topDocs = shorthand();
            if (reusedShorthand) topDocs.sort = {};
            decltype(auto) sort = ::hpp_proto::detail::as_modifiable(ctx, topDocs.sort);
            readSortList<O>(sort, ctx, vit, vend);
          } else if (key == "diversify") {
            auto &topDocs = shorthand();
            topDocs.diversify.reset();
            util::from_json<O>(topDocs.diversify, ctx, vit, vend);
          } else if (key == "collapse") {
            auto &topDocs = shorthand();
            topDocs.collapse.reset();
            util::from_json<O>(topDocs.collapse, ctx, vit, vend);
          } else if (key == "batch_size") {
            util::from_json<O>(shorthand().batch_size, ctx, vit, vend);
          } else if (key == "document_format") {
            util::from_json<O>(shorthand().document_format, ctx, vit, vend);
          } else if (key == "count_limit") {
            util::from_json<O>(shorthand().count_limit, ctx, vit, vend);
          } else if (key == "get_scores") {
            util::from_json<O>(shorthand().get_scores, ctx, vit, vend);
          } else if (key == "min_score") {
            util::from_json<O>(shorthand().min_score, ctx, vit, vend);
          } else {
            ctx.error = error_code::unknown_key;
            return true;
          }
          return bool(ctx.error);
        },
        [](auto &, auto &) {});
    if (td == nullptr || reusedShorthand) {
      return;
    }
    // Shorthand: whatever "ops" carried belongs to the op (sub-ops), and the
    // request's ops map becomes the single shorthand entry.  Runs regardless
    // of ctx.error: the scan can end with the benign end-of-input sentinel
    // set (the root '}' is often the last byte), and on a real error the
    // caller discards the request - packaging is arena-only either way.
    td->ops = value.ops;
    using OpPair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>;
    void *addr = ctx.memory_resource().allocate(sizeof(OpPair), alignof(OpPair));
    auto *pair = new (addr) OpPair{"q", ::hpp_proto::indirect_view<api::SearchOp>(shorthandOp)};
    value.ops = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>(
        std::span<const OpPair>(pair, 1));
    value.json_shorthand = true;
  }
};

} // namespace glz
