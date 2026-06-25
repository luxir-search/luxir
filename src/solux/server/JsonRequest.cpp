#include "JsonRequest.h"

#include <stdexcept>
#include <string>
#include <simdjson.h>

namespace solux {

using namespace simdjson;

void parseQueryRequest(std::string_view body, proto::SearchRequest& out) {
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

  auto& td = *(*out.mutable_ops())["q"].mutable_top_docs();
  auto& m = *td.mutable_query()->mutable_match();
  m.set_field(field);
  m.mutable_val()->set_s(value);

  // Optional response-shape knobs (error-code style: ignored if absent / wrong type).
  int64_t i64;
  if (!root["limit"].get(i64))      td.set_limit(i64);
  if (!root["offset"].get(i64))     td.set_offset(i64);
  if (!root["batch_size"].get(i64)) td.set_batch_size((int32_t)i64);

  bool flag;
  if (!root["count"].get(flag))  td.set_get_number(flag);
  if (!root["scores"].get(flag)) td.set_get_scores(flag);

  dom::array fields;
  if (!root["fields"].get(fields)) {
    for (dom::element f : fields) {
      std::string_view fs;
      if (!f.get_string().get(fs)) td.add_fields(std::string(fs));
    }
  }
}

} // namespace solux
