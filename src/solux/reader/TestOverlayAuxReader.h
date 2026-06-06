#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "AuxReader.h"
#include "protos/solux_types.pb.h"
#include "solux/store/Directory.h"
#include "solux/store/OutputStream.h"
#include "solux/util/log.h"

namespace solux {

/// Cheap test-only overlay kind.  It pins its file at IndexReader open so
/// lifecycle and retry tests can exercise the same path as vector overlays
/// without requiring a vector field or FAISS training.
class TestOverlayAuxReader : public AuxReader {
  std::string name;
  std::shared_ptr<InputFile> file;

public:
  static constexpr std::string_view KIND = "test_overlay";
  static constexpr std::string_view NAME = "test.overlay";

  TestOverlayAuxReader(std::string name, uint64_t gen, uint64_t builtCoreGen,
                       std::shared_ptr<InputFile> file) noexcept
    : AuxReader(gen, builtCoreGen), name(std::move(name)), file(std::move(file)) {}

  std::string_view getKind() const override { return KIND; }
  std::string_view getName() const override { return name; }
  size_t size() const noexcept { return file ? file->size() : 0; }

  static std::shared_ptr<TestOverlayAuxReader> open(Directory& dir,
                                                    const proto::AuxIndexInfo& info,
                                                    bool missingFileOK) {
    if (info.files_size() != 1) {
      throw std::runtime_error(std::format(
        "TestOverlayAuxReader: expected 1 file, got {} for overlay '{}'",
        info.files_size(), info.name()));
    }
    std::string_view fname = info.files(0);
    auto file = dir.openFile(fname, /*expectSynced=*/true);
    if (file == nullptr) {
      if (missingFileOK) {
        LOG_TRACE("TestOverlayAuxReader::open: file {} missing for overlay '{}', will retry",
                  fname, info.name());
        return nullptr;
      }
      throw std::filesystem::filesystem_error(
        std::format("Missing test overlay file '{}' for overlay '{}'", fname, info.name()),
        std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return std::make_shared<TestOverlayAuxReader>(
      std::string(info.name()), info.gen(), info.built_core_gen(), std::move(file));
  }
};

} // namespace solux
