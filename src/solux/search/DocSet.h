#pragma once
#include <vector>

namespace solux {

class DocSet {
public:
  // more efficient later, now just a naive bitset
  std::vector<bool> docs;
};

}
