#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "solux/reader/FieldReader.h"
#include "solux/store/Directory.h"

namespace solux {

// Offline-only builder. The caller must hold exclusive ownership of sidecar
// construction for the index directory; concurrent writers are unsupported.
class BlockBoundsBuilder {
public:
  struct Result {
    std::string fileName;
    uint64_t bytes = 0;
    uint64_t admittedTerms = 0;
    uint64_t groups = 0;
    uint64_t blocks = 0;
    uint64_t scratchBytes = 0;
    float avgdl = 0.0f;
    float envelope = 0.0f;
    double wallSeconds = 0.0;
  };

  static bool eligible(const SegFieldInfo& fieldInfo);
  static Result build(Directory& dir, uint64_t segId, PostingsReader& postingsReader,
                      const SegFieldInfo& fieldInfo);
};

} // namespace solux
