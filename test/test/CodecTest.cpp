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
  } else if (name=="SoluxPFOR") {
    return std::make_unique<SoluxPFOR>();
  } else if (name=="SoluxPFORd") {
    return std::make_unique<SoluxPFORd>();
  } else if (name=="SIMDFor") {
    return std::make_unique<IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFrameOfReference>>();
  } else if (name=="ForCODEC") {
    return std::make_unique<IntegerCODECTypeWrapper<SIMDCompressionLib::ForCODEC>>();
  } else if (name=="SoluxFor") {
    return std::make_unique<SoluxFor>();
  } else if (name=="SoluxSIMDFor") {
    return std::make_unique<SoluxSIMDFor>();
  }

  return {};
}

} // end namespace
