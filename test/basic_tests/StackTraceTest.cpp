// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <link.h>
#include <string_view>
#include <gtest/gtest.h>
#include "luxir/server/LuxirError.h"

namespace {

bool sharedUnwinderLoaded() {
  bool loaded = false;
  dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
    if (std::string_view(info->dlpi_name).find("libgcc_s.so") != std::string_view::npos) {
      *static_cast<bool*>(data) = true;
    }
    return 0;
  }, &loaded);
  return loaded;
}

} // namespace

TEST(StackTraceTest, retainsAddressesWithoutLoadingSharedUnwinder) {
  bool loadedBefore = sharedUnwinderLoaded();
  auto trace = luxir::getStackTrace();
  EXPECT_NE(std::string::npos, trace.find("[0x")) << trace;
  // Native builds may already link libgcc_s; static builds must not load it
  // merely to report an internal update failure.
  EXPECT_EQ(loadedBefore, sharedUnwinderLoaded());
}
