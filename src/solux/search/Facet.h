#pragma once
#include <string_view>
#include <vector>

#include "IndexReader.h"

namespace solux {
class FacetReq {
  IndexReader& reader;
  std::string_view fieldName;
public:
  std::vector<std::vector<bool>> allMatches;

  FacetReq(IndexReader& reader, std::string_view fieldName) : reader(reader), fieldName(fieldName) {
    allMatches.resize(reader.segments().size());
  }
};


}
