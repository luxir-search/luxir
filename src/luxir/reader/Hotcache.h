// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"

namespace luxir {

/// The byte ranges of a segment's files that opening the segment reads, so a
/// backend that fetches ranges lazily can fetch them together instead of one
/// small request at a time.
///
/// It is stored as one more file of the segment, `s<seg>__hot`, written when
/// the segment's files are finalized and listed in the commit like any other
/// segment file, so verify-on-open, the scrub, replication and deletion carry
/// it without special cases. Segment data files are never changed. A segment
/// without one (written before, or by a backend that does not need it) simply
/// opens with small requests.
///
/// Layout: the 8-byte magic "LUXHOT01", vint format version, vint count, then
/// `count` triples of vints (file number, offset, length) sorted by file number
/// and offset, then the xxh3 of everything before it as 8 little-endian bytes.
class Hotcache {
public:
  struct Entry {
    uint32_t fileNum = 0;
    ByteRange range;
  };

  static constexpr std::string_view kMagic = "LUXHOT01";
  // Recorded ranges are rounded out to this, which is also finer than any sensible chunk size.
  static constexpr uint64_t kAlign = 4096;

  std::vector<Entry> entries;  // sorted by (fileNum, offset), non-overlapping

  /// "s<seg>__hot" for the segment (segId's sortable string is the key).
  static std::string fileName(uint64_t segId);
  static std::string fileName(std::string_view segStr);

  /// If `name` is a segment data file ("s<seg>_<filenum>"), its segment string
  /// (the part after the 's') and file number.
  struct DataFile {
    std::string_view segStr;
    uint32_t fileNum = 0;
  };
  static std::optional<DataFile> parseDataFile(std::string_view name);

  std::string encode() const;
  /// Throws std::runtime_error on a malformed or corrupt file.
  static Hotcache decode(std::string_view bytes);

  /// The ranges of one file, or empty.
  std::vector<ByteRange> rangesOf(uint32_t fileNum) const;

  /// Opens the finished segment through a recording directory and returns the
  /// ranges the open read, merged and rounded to kAlign. Needs every file of
  /// the segment in `dir`.
  static Hotcache record(Directory& dir, uint64_t segId);

  /// record() + the `s<seg>__hot` file; returns its descriptor.
  static FileDescriptor write(Directory& dir, uint64_t segId);
};

/// For a directory that fetches remotely, writes the segment's hotcache and
/// appends its name and descriptor to the lists a finished segment reports.
/// Other directories do nothing: their open reads are local. A failure to
/// record is logged and leaves the segment without a hotcache.
void writeHotcacheIfRemote(Directory& dir, uint64_t segId, std::vector<std::string>* filenames,
                           std::vector<FileDescriptor>* descriptors);

/// A directory whose input files record the ranges read through them. Reads of
/// the whole file (read(), getInputStream()) record the whole file.
class RecordingDirectory : public DelegatingDirectory {
public:
  using Ranges = std::map<std::string, std::vector<ByteRange>, std::less<>>;

private:
  std::mutex mutex;
  Ranges ranges_;

  class File;

public:
  explicit RecordingDirectory(Directory& inner);

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override;

  /// Everything recorded so far, by file name.
  Ranges recorded();
  void add(const std::string& name, ByteRange range);
};

}  // namespace luxir
