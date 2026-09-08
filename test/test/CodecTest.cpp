// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CodecTest.h"

namespace luxir {

std::unique_ptr<U32Codec> U32CodecFactory::getCodec(const std::string& name) {
  if (name=="SimpleCodec") {
    return std::make_unique<SimpleCodec>();
  } else if (name=="LuxirPFOR") {
    return std::make_unique<LuxirPFOR>();
  } else if (name=="LuxirPFORd") {
    return std::make_unique<LuxirPFORd>();
  } else if (name=="LuxirSIMDFor") {
    return std::make_unique<LuxirSIMDFor>();
  }

  return {};
}

} // end namespace
