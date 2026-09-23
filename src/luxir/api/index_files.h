// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "build.h"
#include "luxir_index.hpp"
#include "luxir/store/OutputStream.h"
namespace luxir {
inline std::vector<FileDescriptor> fromWire(std::span<const api::FileDescriptor> files) {
  std::vector<FileDescriptor> result;
  result.reserve(files.size());
  for (const auto& file : files) result.push_back({std::string(file.name), file.size, file.xxh3});
  return result;
}

inline std::span<const api::FileDescriptor> toWire(
    std::span<const FileDescriptor> files, std::pmr::memory_resource& mr) {
  std::span<const api::FileDescriptor> result;
  auto* out = api::build::allocArray(result, files.size(), mr);
  for (size_t i = 0; i < files.size(); i++) out[i] = {files[i].name, files[i].size, files[i].xxh3};
  return result;
}


inline std::vector<FileDescriptor> filesOf(const api::IndexInfo& info) {
  std::vector<FileDescriptor> result;
  auto append = [&](auto files) {
    for (const auto& f : files) result.push_back({std::string(f.name), f.size, f.xxh3});
  };
  for (const auto& seg : info.segments) {
    append(seg.files);
    for (const auto& aux : seg.overlays) append(aux.files);
  }
  for (const auto& aux : info.aux_indexes) append(aux.files);
  return result;
}
} // namespace luxir
