#include "JsonRequest.h"

#include <memory_resource>
#include <stdexcept>
#include <string>

namespace solux {

void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena) {
  std::string err;
  if (!solux::api::read_json(out, body, arena, &err)) {
    throw std::runtime_error(err.empty() ? "invalid JSON" : err);
  }
}

} // namespace solux
