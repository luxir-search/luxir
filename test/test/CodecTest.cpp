#include "CodecTest.h"
#include "simdcomp/include/codecfactory.h"


namespace solux {

std::unique_ptr<U32Codec> U32CodecFactory::getCodec(const std::string& name) {
  if (name=="FastPFor") {
    return std::make_unique<IntegerCODECTypeWrapper<SIMDCompressionLib::FastPFor<4, false>>>(); // corresponds to a block size of 128 and non-delta coding
  } else if (name=="SIMDFastPFor") {
    return std::make_unique<IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFastPFor<4>>>();
  } else if (name=="SIMDFastPForDelta1") {
    return std::make_unique<IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFastPFor<4, SIMDCompressionLib::RegularDeltaSIMD>>>();
  } else if (name=="SimpleCodec") {
    return std::make_unique<SimpleCodec>();
  }
  return {};
}

} // end namespace
