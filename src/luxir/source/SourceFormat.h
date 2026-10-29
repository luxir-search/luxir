// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The _source file format (see docs/design/source-store.md).  Nothing here may
// depend on the schema, the segment format or the manifest, so that a
// standalone tool can read a source file on its own.  The format is meant to
// freeze, growing only through new codec ids, record flag bits and block kinds
// (a reader skips any block kind it does not know), but it is EXPERIMENTAL for
// now: there is no compatibility promise until luxir-source-dump reads it and
// golden files and a fuzzer pin it down.  The Map record body also still uses
// the live luxir.Map encoding, which needs a frozen copy of its own first.
//
// Integers are little-endian and varints are LEB128.  "xxh3/32" is the low 32
// bits of XXH3-64.
//
//   file header:
//     "LXSOURCE" | u32 format=1 | 16-byte file id | 16-byte sync marker
//     | u64 created_ms | varint origin len + bytes | u32 header xxh3/32
//   dictionary block:
//     sync | u8 kind=1 | u32 dict_id | u32 len | dict bytes | u64 checksum
//     (the checksum covers dict_id through the dict bytes)
//   data block:
//     sync | u8 kind=2 | u8 codec (0 none, 1 zstd, 2 lz4) | u32 dict_id
//     | u32 first_ordinal | u32 n_docs | u32 raw_len | u32 comp_len
//     | u32 header xxh3/32 | payload | u64 checksum
//   footer (an accelerator only, rebuilt by scanning when it does not verify):
//     (u32 dict_id, u64 offset) * n_dicts | (u32 first_ordinal, u64 offset) * n_blocks
//     | u32 n_dicts | u32 n_blocks | u32 doc_count
//     | u64 xxh3 of all preceding footer bytes | u32 footer length | "LXSRCEND"
//
// The header checksum covers the bytes from `kind` through `comp_len`.  A block
// checksum is XXH3-64 of the payload seeded with XXH3-64(file id | u64 block
// offset), so a block copied to another place fails like random corruption.
// The decompressed data payload is column-wise:
//   varint n | n * varint id_len | n * varint body_len | n * varint version
//   | n * u8 flags | id bytes... | body bytes...

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;
struct ZSTD_DDict_s;
struct ZSTD_CDict_s;

namespace luxir::source {

inline constexpr std::string_view kMagic = "LXSOURCE";
inline constexpr std::string_view kEndMagic = "LXSRCEND";
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr size_t kSyncBytes = 16;
inline constexpr size_t kDataHeaderBytes = kSyncBytes + 1 + 1 + 4 + 4 + 4 + 4 + 4 + 4;
inline constexpr size_t kChecksumBytes = 8;
// Sanity ceiling applied while parsing untrusted bytes.
inline constexpr uint32_t kMaxRawBlock = 1u << 30;

enum class BlockKind : uint8_t { DICT = 1, DATA = 2 };
enum class Codec : uint8_t { NONE = 0, ZSTD = 1, LZ4 = 2 };

// Record flag bits.  The other bits are reserved.
inline constexpr uint8_t kFlagMinifiedJson = 1;  // body is raw JSON, whitespace outside strings dropped
inline constexpr uint8_t kFlagProtoMap = 2;      // body is a serialized luxir.Map
inline constexpr uint8_t kFlagTombstone = 4;     // id only, no body
inline constexpr uint8_t kFlagAllowDups = 8;     // indexed with allow_dups: scan never deduplicates
inline constexpr uint8_t kFlagMissing = 16;      // placeholder: this doc was indexed without a source

struct Record {
  std::string_view id;
  std::string_view body;
  uint64_t version = 0;
  uint8_t flags = 0;
};

struct FileId {
  std::array<uint8_t, 16> bytes{};
  bool operator==(const FileId&) const = default;
};

struct Header {
  uint32_t format = kFormatVersion;
  FileId fileId;
  std::array<uint8_t, kSyncBytes> sync{};
  uint64_t createdMs = 0;
  std::string origin;
  size_t size = 0;  // encoded length in bytes
};

// Location of one data block.
struct BlockRef {
  uint32_t firstOrdinal = 0;
  uint32_t nDocs = 0;
  uint64_t offset = 0;
};

// Raw (decompressed) payload of one data block, parsed on demand.
class DecodedBlock {
public:
  uint32_t firstOrdinal = 0;
  uint32_t nDocs = 0;

  // Takes the raw payload and indexes it.  False if it is internally inconsistent.
  bool parse(std::string payload, uint32_t first, uint32_t n);
  Record record(uint32_t index) const;
  size_t rawBytes() const { return raw.size(); }

private:
  std::string raw;
  std::vector<uint32_t> idOffset;    // n + 1 entries
  std::vector<uint32_t> bodyOffset;  // n + 1 entries
  std::vector<uint64_t> versions;
  std::vector<uint8_t> flags;
};

// Compression context reused across blocks.  Not thread safe.
class Codecs {
public:
  Codecs();
  ~Codecs();
  Codecs(const Codecs&) = delete;
  Codecs& operator=(const Codecs&) = delete;

  size_t compress(Codec codec, int level, const ZSTD_CDict_s* dict, std::string_view src, std::string& dst);
  bool decompress(Codec codec, const ZSTD_DDict_s* dict, std::string_view src, size_t rawLen, std::string& dst);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

struct DictDeleter {
  void operator()(ZSTD_DDict_s* dict) const;
  void operator()(ZSTD_CDict_s* dict) const;
};
using DecompressDict = std::unique_ptr<ZSTD_DDict_s, DictDeleter>;
using CompressDict = std::unique_ptr<ZSTD_CDict_s, DictDeleter>;

// Receives the bytes of a file being written, in order.
class Sink {
public:
  virtual ~Sink() = default;
  virtual void write(const char* data, size_t len) = 0;
};

struct WriterOptions {
  Codec codec = Codec::ZSTD;
  int zstdLevel = 3;
  uint32_t blockBytes = 16384;   // raw bytes at which a block is sealed
  uint32_t maxDocsPerBlock = 0;  // records at which a block is sealed; 0 = bytes alone decide
};

// Streams one source file.  Records get ordinals 0, 1, 2 ... in add order.
class FileWriter {
public:
  FileWriter(Sink& sink, const WriterOptions& options, std::string_view origin, uint64_t createdMs);
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  // Registers a dictionary used by later blocks.  Written immediately.
  void addDictionary(uint32_t dictId, std::string_view bytes);

  // Selects the dictionary (0 = none) for blocks sealed from now on.
  void useDictionary(uint32_t dictId);

  void add(const Record& record);

  // Copies a block of another file as it is, with only its ordinal rebased.  The
  // pending records are sealed first.  `payload` is the compressed payload.
  void addRawBlock(Codec codec, uint32_t dictId, uint32_t nDocs, uint32_t rawLen, std::string_view payload);

  // Seals the pending block, then writes the footer.  The writer is done after.
  void finish();

  uint32_t docCount() const { return nextOrdinal; }
  uint64_t bytesWritten() const { return offset; }
  const Header& header() const { return head; }
  // Heap held by the pending records and the scratch buffers, for RAM accounting.
  size_t pendingBytes() const;

private:
  Sink& sink;
  WriterOptions options;
  Header head;
  Codecs codecs;
  uint64_t offset = 0;
  uint32_t nextOrdinal = 0;
  uint32_t blockFirst = 0;
  uint32_t activeDict = 0;
  const ZSTD_CDict_s* activeDictPtr = nullptr;
  bool finished = false;

  // Pending records, column-wise.
  std::string ids;
  std::string bodies;
  std::vector<uint32_t> idLens;
  std::vector<uint32_t> bodyLens;
  std::vector<uint64_t> versions;
  std::vector<uint8_t> flags;
  std::string scratch;
  std::string compressed;
  std::vector<BlockRef> blocks;
  std::vector<std::pair<uint32_t, CompressDict>> dicts;
  std::vector<std::pair<uint32_t, uint64_t>> dictOffsets;

  void emit(const char* data, size_t len);
  void emitBlock(Codec codec, uint32_t dictId, uint32_t first, uint32_t nDocs, uint32_t rawLen,
                 std::string_view payload);
  void seal();
};

// Read side over the bytes of a whole file (mapped or in memory).  The bytes
// must outlive the reader.
class FileReader {
public:
  // Verifies the header and builds the block index from the footer, or by
  // scanning when the footer does not verify.  nullopt (with `error`) if the
  // header itself is unusable.
  static std::optional<FileReader> open(std::string_view data, std::string* error = nullptr);

  const Header& header() const { return head; }
  uint32_t docCount() const { return docs; }
  const std::vector<BlockRef>& blocks() const { return index; }
  bool footerVerified() const { return footerOk; }

  // Index of the block holding `ordinal`, or -1.  `hint` is the block a previous
  // lookup found; it and its successor are tried before the binary search.
  int64_t blockFor(uint32_t ordinal, int64_t hint = -1) const;

  // Verifies and decompresses block `i`.  False if it is damaged.
  bool readBlock(size_t i, Codecs& codecs, DecodedBlock& out) const;

  // The still-compressed payload of block `i` after verifying it, for verbatim copies.
  struct RawBlock {
    Codec codec = Codec::NONE;
    uint32_t dictId = 0;
    uint32_t nDocs = 0;
    uint32_t rawLen = 0;
    std::string_view payload;
  };
  bool rawBlock(size_t i, RawBlock& out) const;

  // Dictionary bytes by id (as written in this file), or null.
  const std::string* dictionary(uint32_t dictId) const;

  // Result of walking every block of a file, resynchronizing past damage.
  struct ScanResult {
    std::vector<BlockRef> blocks;
    std::vector<std::pair<uint32_t, std::string>> dictionaries;
    uint64_t skippedBytes = 0;
    uint32_t badBlocks = 0;
  };
  static ScanResult scan(std::string_view data, const Header& header);

  // Parses and verifies the file header at the start of `data`.
  static std::optional<Header> parseHeader(std::string_view data, std::string* error = nullptr);

private:
  std::string_view data;
  Header head;
  std::vector<BlockRef> index;
  std::vector<std::pair<uint32_t, std::string>> dicts;
  std::vector<std::pair<uint32_t, DecompressDict>> decompressDicts;
  uint32_t docs = 0;
  bool footerOk = false;

  const ZSTD_DDict_s* decompressDict(uint32_t dictId) const;
};

// Seed for the checksum of a block that starts at `blockOffset` in a file.
uint64_t blockSeed(const FileId& fileId, uint64_t blockOffset);

}  // namespace luxir::source
