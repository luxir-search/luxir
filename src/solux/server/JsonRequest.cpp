#include "JsonRequest.h"

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <variant>
#include <simdjson.h>

#include "solux/api/build.h"

namespace solux {

using namespace simdjson;

void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena) {
  dom::parser parser;
  padded_string json(body);  // copies + pads to simdjson's required alignment

  dom::element rootEl;
  if (auto e = parser.parse(json).get(rootEl)) {
    throw std::runtime_error(std::string("invalid JSON: ") + error_message(e));
  }
  dom::object root;
  if (rootEl.get(root)) throw std::runtime_error("request body must be a JSON object");

  // query.match (required) -> { "<field>": "<value>" }
  dom::element queryEl;
  if (root["query"].get(queryEl)) throw std::runtime_error("missing 'query'");
  dom::object query;
  if (queryEl.get(query)) throw std::runtime_error("'query' must be an object");
  dom::object match;
  if (query["match"].get(match)) throw std::runtime_error("phase 0 supports only a 'match' query");
  if (match.size() == 0) throw std::runtime_error("'match' must name a field");

  auto kv = *match.begin();
  std::string_view field = kv.key;
  std::string_view value;
  if (kv.value.get_string().get(value)) throw std::runtime_error("'match' value must be a string");

  namespace api = solux::api;
  // Build a NON-OWNING SearchRequest into `arena`: ops["q"] -> SearchOp{TopDocs}, with the
  // match query + value allocated in the arena. Strings from the parsed (transient) JSON
  // are copied via arenaStr so they outlive this call. Allocations are placement-new on
  // arena bytes; every solux::api type is trivially destructible (the arena is dropped).
  using OpPair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>;
  auto* op = new (arena.allocate(sizeof(api::SearchOp), alignof(api::SearchOp))) api::SearchOp();
  auto* opsArr = new (arena.allocate(sizeof(OpPair), alignof(OpPair)))
      OpPair{"q", ::hpp_proto::indirect_view<api::SearchOp>(op)};
  out.ops = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>(
      std::span<const OpPair>(opsArr, 1));

  auto& td = op->kind.emplace<api::TopDocs>();

  auto* q = new (arena.allocate(sizeof(api::Query), alignof(api::Query))) api::Query();
  auto& m = q->kind.emplace<api::Match>();
  m.field = api::build::arenaStr(arena, field);
  auto* v = new (arena.allocate(sizeof(api::Val), alignof(api::Val))) api::Val();
  v->kind.emplace<std::string_view>(api::build::arenaStr(arena, value));
  m.val = v;     // optional_indirect_view<Val> from Val*
  td.query = q;  // optional_indirect_view<Query> from Query*

  // Optional response-shape knobs (error-code style: ignored if absent / wrong type).
  int64_t i64;
  if (!root["limit"].get(i64))      td.limit = i64;
  if (!root["offset"].get(i64))     td.offset = i64;
  if (!root["batch_size"].get(i64)) td.batch_size = (int32_t)i64;

  bool flag;
  if (!root["count"].get(flag))  td.get_number = flag;
  if (!root["scores"].get(flag)) td.get_scores = flag;

  dom::array fields;
  if (!root["fields"].get(fields)) {
    api::build::SpanBuilder<std::string_view> fb(arena);
    for (dom::element f : fields) {
      std::string_view fs;
      if (!f.get_string().get(fs)) fb.push_back(api::build::arenaStr(arena, fs));
    }
    td.fields = fb.finish();
  }
}

} // namespace solux
