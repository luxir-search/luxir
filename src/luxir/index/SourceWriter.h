// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luxir/reader/Postings.h"
#include "luxir/schema/SourceSettings.h"
#include "luxir/source/SourceFormat.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

// Streams the bytes of a source file into a directory file.
class SourceFileSink final : public source::Sink {
public:
  SourceFileSink(Directory& dir, const std::string& name, std::optional<uint64_t> expectedSize = std::nullopt)
      : dir(dir) {
    Directory::FileCreateOptions options;
    options.expectedSize = expectedSize;
    file = dir.createFile(name, options);
    if (!file) throw std::runtime_error("cannot create source file " + name);
    out = std::make_unique<OutputStream>(file.get());
  }

  void write(const char* data, size_t len) override { out->write(data, len); }

  // Closes and publishes the file; the descriptor is for the manifest.
  FileDescriptor finish() {
    out->close();
    dir.finishFile(*file);
    return file->descriptor();
  }

private:
  Directory& dir;
  std::unique_ptr<File> file;
  std::unique_ptr<OutputStream> out;
};

// Collects one flushed segment's documents as they are indexed and writes them to
// a source file when the segment flushes.  Record i belongs to doc ID i, so a
// doc without a captured source (indexed without one) gets a placeholder record.
class SourceWriter final {
public:
  SourceWriter(const SourceSettings& settings, std::string_view origin)
      : writer(std::make_unique<source::FileWriter>(sink, settings.writerOptions(), origin, nowMillis())),
        blockBytes(settings.blockBytes) {}

  void add(std::string_view id, std::string_view body, uint64_t version, uint8_t flags) {
    writer->add({id, body, version, flags});
  }

  // Gives every doc below `docs` that has no record a placeholder.
  void padTo(uint32_t docs) {
    while (writer->docCount() < docs) writer->add({{}, {}, 0, source::kFlagMissing});
  }

  uint32_t docCount() const { return writer->docCount(); }

  // Bytes held: the compressed blocks plus, at most, the block being filled.
  size_t ramBytes() const { return sink.bytes.capacity() + blockBytes; }

  // Writes the file as the source file `name`.  The writer is spent afterwards.
  source::Header flush(Directory& dir, const std::string& name, FileDescriptor& descriptor) {
    writer->finish();
    SourceFileSink file(dir, name, sink.bytes.size());
    file.write(sink.bytes.data(), sink.bytes.size());
    descriptor = file.finish();
    return writer->header();
  }

private:
  struct BytesSink final : source::Sink {
    std::string bytes;
    void write(const char* data, size_t len) override { bytes.append(data, len); }
  };
  BytesSink sink;
  std::unique_ptr<source::FileWriter> writer;
  size_t blockBytes;

  static uint64_t nowMillis() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }
};

}  // namespace luxir
