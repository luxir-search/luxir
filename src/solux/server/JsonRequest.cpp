#include "JsonRequest.h"

#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace solux {

void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena) {
  namespace api = solux::api;

  // Full form first: {"ops": {...}}. A bare {"ops": ...} body parses as either form
  // (TopDocs also has sub-ops), so trying SearchRequest first makes the full form
  // win; any root shorthand key ("query", "limit", ...) fails it immediately and
  // routes below.
  std::string fullErr;
  if (api::read_json(out, body, arena, &fullErr)) return;

  // Shorthand: the root object IS a single top_docs op, registered as ops["q"]:
  //   {"query": {"match": {"title_w": "dune"}}, "limit": 10, "fields": ["id"]}
  auto* op = new (arena.allocate(sizeof(api::SearchOp), alignof(api::SearchOp))) api::SearchOp();
  auto& td = op->kind.emplace<api::TopDocs>();
  std::string shortErr;
  if (!api::read_json(td, body, arena, &shortErr)) {
    // The shorthand error is usually the useful one (it read past the root keys);
    // fall back to the full-form error if the shorthand produced none.
    throw std::runtime_error(!shortErr.empty() ? shortErr
                             : !fullErr.empty() ? fullErr
                                                : "invalid JSON");
  }
  using OpPair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>;
  auto* opsArr = new (arena.allocate(sizeof(OpPair), alignof(OpPair)))
      OpPair{"q", ::hpp_proto::indirect_view<api::SearchOp>(op)};
  out = api::SearchRequest{};  // the failed full-form parse may have left partial state
  out.ops = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>(
      std::span<const OpPair>(opsArr, 1));
}

} // namespace solux
