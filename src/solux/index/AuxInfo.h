#pragma once
// Owning, durable representation of aux-index metadata.
//
// The wire type solux::api::AuxIndexInfo is NON-OWNING (string_view / span / bytes_view),
// so the engine cannot use it as persistent or mutated state. IndexWriter keeps its
// aux-index state (current aux indexes, per-segment overlays) in this owning struct and
// converts at the wire boundary only: decode -> fromWire (copy out), build -> toWire
// (views over the owning struct, span backing allocated in a scratch arena).
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <hpp_proto/field_types.hpp>  // bytes_view

#include "solux/api/build.h"
#include "solux/api/solux_types.hpp"

namespace solux {

struct AuxInfo {
  std::string kind;
  std::string field;
  std::string name;
  std::uint64_t gen = 0;
  std::uint64_t commit_time = 0;
  std::vector<std::string> files;
  std::vector<std::byte> opaque_meta;  // raw bytes
  std::uint64_t built_core_gen = 0;
};

// Copy a decoded (non-owning) AuxIndexInfo into an owning AuxInfo.
inline AuxInfo fromWire(const solux::api::AuxIndexInfo& w) {
  AuxInfo a;
  a.kind = std::string(w.kind);
  a.field = std::string(w.field);
  a.name = std::string(w.name);
  a.gen = w.gen;
  a.commit_time = w.commit_time;
  a.files.reserve(w.files.size());
  for (auto f : w.files) {
    a.files.emplace_back(f);
  }
  a.opaque_meta.assign(w.opaque_meta.data(), w.opaque_meta.data() + w.opaque_meta.size());
  a.built_core_gen = w.built_core_gen;
  return a;
}

// Build a non-owning AuxIndexInfo viewing `a` (strings) with its files span allocated
// in `mr` (pointing at a's std::strings). `a` and `mr` must outlive the returned view
// and any serialization of it.
inline solux::api::AuxIndexInfo toWire(const AuxInfo& a, std::pmr::memory_resource& mr) {
  solux::api::AuxIndexInfo w;
  w.kind = a.kind;
  w.field = a.field;
  w.name = a.name;
  w.gen = a.gen;
  w.commit_time = a.commit_time;
  std::string_view* files = solux::api::build::allocArray(w.files, a.files.size(), mr);
  for (std::size_t i = 0; i < a.files.size(); i++) {
    files[i] = a.files[i];
  }
  w.opaque_meta = ::hpp_proto::bytes_view(a.opaque_meta.data(), a.opaque_meta.size());
  w.built_core_gen = a.built_core_gen;
  return w;
}

}  // namespace solux
