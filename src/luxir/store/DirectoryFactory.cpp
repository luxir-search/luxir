// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/DirectoryFactory.h"
#include <glaze/glaze.hpp>

namespace luxir {

std::optional<std::string> CollectionStorage::current(Directory& container) {
  auto file = container.openFile("CURRENT");
  if (!file) return std::nullopt;
  Selection result;
  std::string text(file->read()); // the parser reads up to a terminator
  if (glz::read_json(result, text) || result.incarnation.empty()
      || result.incarnation.find_first_of("/\\") != std::string::npos
      || result.incarnation == "." || result.incarnation == "..") {
    throw std::runtime_error("Invalid collection CURRENT");
  }
  return std::move(result.incarnation);
}

void CollectionStorage::select(std::string_view incarnation) {
  auto container = factory->container(id_, false);
  auto bytes = glz::write_json(Selection{std::string(incarnation)}).value();
  Directory::FileCreateOptions options; options.expectedSize = bytes.size();
  auto file = container->createFile("CURRENT.pending", options);
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size()); out.close();
  container->finishFile(*file);
  std::array<std::string, 1> pending{"CURRENT.pending"};
  container->sync(pending);
  container->renameFile("CURRENT.pending", "CURRENT");
  std::array<std::string, 1> directory{"."};
  container->sync(directory);
}

} // namespace luxir
