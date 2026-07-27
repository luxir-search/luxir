#pragma once

#include <memory>
#include <string>

#include "solux/codec/Codec.h"

namespace solux {

class U32CodecFactory {
public:
  static std::unique_ptr <U32Codec> getCodec(const std::string &name);
};

} // end namespace