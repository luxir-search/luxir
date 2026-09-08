// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "luxir/codec/Codec.h"

namespace luxir {

class U32CodecFactory {
public:
  static std::unique_ptr <U32Codec> getCodec(const std::string &name);
};

} // end namespace