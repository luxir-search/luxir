// JSON codec metadata for the solux::api wire model: the glaze from/to overrides that the
// generated glz::meta cannot express, plus the Solux JSON dialect.
//
// Split out of solux_types.hpp because glaze is expensive to parse (~1.5s of frontend per TU)
// and solux_types.hpp is reached by nearly every engine TU through Schema.h / Query.h. Only
// TUs that actually serialize include this: the generated .json.cpp files (force-included by
// protos/CMakeLists.txt) and the handful of server/test TUs that call glz:: directly. TUs that
// only call the out-of-line solux::api::read_json / write_json entry points do NOT need it.
//
// Every such TU must include this header, not <hpp_proto/json.hpp> alone: the specializations
// below and in json_dialect.h are what make the dialect consistent, and instantiating glaze
// over an api type without them is an ODR violation.
#pragma once

#include "solux_types.hpp"

#include <hpp_proto/json.hpp>

namespace glz {
template <>
struct to<JSON, google::protobuf::NullValue> {
  template <auto Opts>
  GLZ_ALWAYS_INLINE static void op(auto && /*value*/, auto &&...args) {
    serialize<JSON>::template op<Opts>(std::monostate{}, std::forward<decltype(args)>(args)...);
  }
};
template <>
struct from<JSON, google::protobuf::NullValue> {
  template <auto Opts>
  GLZ_ALWAYS_INLINE static void op(auto &value, auto &&...args) {
    parse<JSON>::template op<Opts>(std::monostate{}, std::forward<decltype(args)>(args)...);
    value = google::protobuf::NullValue::NULL_VALUE;
  }
};

// optional_indirect_view<T> is the non-owning singular optional message field (TopDocs.query,
// Match.val, ...). hpp-proto only ships glz for the optional_indirect_view_REF wrapper; the
// generated glz binds the raw member, so add from/to for the raw view here (arena-allocate on
// read, mirroring hpp-proto's optional_indirect_view_ref handler).
template <typename Type>
struct from<JSON, ::hpp_proto::optional_indirect_view<Type>> {
  template <auto Opts>
  static void op(auto &value, ::hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    if (!util::parse_null<Opts>(value, ctx, it, end)) {
      void *addr = ctx.memory_resource().allocate(sizeof(Type), alignof(Type));
      auto *obj = new (addr) Type; // NOLINT(cppcoreguidelines-owning-memory)
      parse<JSON>::template op<Opts>(*obj, ctx, it, end);
      value = obj;
    }
  }
};
template <typename Type>
struct to<JSON, ::hpp_proto::optional_indirect_view<Type>> {
  template <auto Opts, class... Args>
  GLZ_ALWAYS_INLINE static void op(auto &&value, Args &&...args) noexcept {
    if (value.has_value()) {
      to<JSON, Type>::template op<Opts>(*value, std::forward<Args>(args)...);
    }
  }
};
} // namespace glz

// Solux JSON dialect: hand from/to<JSON> overrides of the generated glz::meta
// (untagged Val, flattened Map, bare-array Vector, ...).
#include "json_dialect.h"
