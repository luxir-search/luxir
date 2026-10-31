// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "luxir/server/TenantCatalog.h"

namespace luxir::test {
inline std::string selectedIncarnation(const std::filesystem::path& path, std::string_view name = "main",
                                       std::string_view tenant = CollectionId::kDefaultTenant) {
  FSDirFactory factory(path, true);
  return TenantCatalog(factory, tenant).current(name).value();
}
inline std::vector<std::string> catalogIncarnations(const std::filesystem::path& path, std::string_view name,
                                                   std::string_view tenant = CollectionId::kDefaultTenant) {
  FSDirFactory factory(path, true);
  return TenantCatalog(factory, tenant).incarnations(name);
}
inline std::filesystem::path incarnationPath(const std::filesystem::path& path, std::string_view name = "main",
                                            std::string_view tenant = CollectionId::kDefaultTenant) {
  return path / "t" / tenant / "i" / selectedIncarnation(path, name, tenant);
}
}
