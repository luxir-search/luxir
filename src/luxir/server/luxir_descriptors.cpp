// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir_descriptors.h"

#include "luxir_fds_data.h" // generated: luxir::detail::kLuxirFds / kLuxirFdsLen

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <string>
#include <vector>

namespace luxir {

void registerLuxirDescriptors() {
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromArray(detail::kLuxirFds, static_cast<int>(detail::kLuxirFdsLen))) {
    return;
  }
  // InternalAddGeneratedFile keeps the pointer for lazy parsing, so the encoded
  // bytes must outlive use: stash them in a function-local static.
  static std::vector<std::string> kept;
  for (const auto &file : fds.file()) {
    if (google::protobuf::DescriptorPool::generated_pool()->FindFileByName(file.name()) != nullptr) {
      continue; // well-known types (and re-entrant calls) already registered
    }
    kept.emplace_back();
    file.SerializeToString(&kept.back());
    google::protobuf::DescriptorPool::InternalAddGeneratedFile(kept.back().data(),
                                                               static_cast<int>(kept.back().size()));
  }
}

} // namespace luxir
