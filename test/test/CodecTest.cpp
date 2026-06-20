#include "CodecTest.h"

namespace solux {

std::unique_ptr<U32Codec> U32CodecFactory::getCodec(const std::string& name) {
  if (name=="SimpleCodec") {
    return std::make_unique<SimpleCodec>();
  } else if (name=="SoluxPFOR") {
    return std::make_unique<SoluxPFOR>();
  } else if (name=="SoluxPFORd") {
    return std::make_unique<SoluxPFORd>();
  } else if (name=="SoluxSIMDFor") {
    return std::make_unique<SoluxSIMDFor>();
  }

  return {};
}

} // end namespace
