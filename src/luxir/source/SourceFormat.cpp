// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceFormat.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <random>
#include <stdexcept>

#include <lz4.h>
#include <xxhash.h>
#include <zstd.h>

namespace luxir::source {

static_assert(std::endian::native == std::endian::little, "the source format is little-endian");

namespace {

void put32(std::string& out, uint32_t v) { out.append((const char*)&v, 4); }
void put64(std::string& out, uint64_t v) { out.append((const char*)&v, 8); }

uint32_t get32(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t get64(const char* p) { uint64_t v; memcpy(&v, p, 8); return v; }

void putVarint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back((char)(v | 0x80));
    v >>= 7;
  }
  out.push_back((char)v);
}

// Reads a varint from [pos, end); false if truncated, over 10 bytes, or
// carrying bits beyond 64 in the tenth byte.
bool getVarint(const char*& pos, const char* end, uint64_t& out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= end) return false;
    uint8_t b = (uint8_t)*pos++;
    if (shift == 63 && (b & 0x7e) != 0) return false;
    v |= (uint64_t)(b & 0x7f) << shift;
    if ((b & 0x80) == 0) { out = v; return true; }
  }
  return false;
}

uint32_t lowChecksum(const char* data, size_t len) { return (uint32_t)XXH3_64bits(data, len); }

uint64_t payloadChecksum(const FileId& id, uint64_t offset, const char* data, size_t len) {
  return XXH3_64bits_withSeed(data, len, blockSeed(id, offset));
}

}  // namespace

uint64_t blockSeed(const FileId& fileId, uint64_t blockOffset) {
  char buf[24];
  memcpy(buf, fileId.bytes.data(), 16);
  memcpy(buf + 16, &blockOffset, 8);
  return XXH3_64bits(buf, sizeof(buf));
}

void DictDeleter::operator()(ZSTD_DDict_s* dict) const { ZSTD_freeDDict(dict); }
void DictDeleter::operator()(ZSTD_CDict_s* dict) const { ZSTD_freeCDict(dict); }

// ---------------------------------------------------------------- DecodedBlock

bool DecodedBlock::parse(std::string payload, uint32_t first, uint32_t n) {
  firstOrdinal = first;
  nDocs = n;
  raw = std::move(payload);
  const char* pos = raw.data();
  const char* end = pos + raw.size();
  uint64_t count = 0;
  if (!getVarint(pos, end, count) || count != n) return false;
  // Every record takes at least four bytes (three one-byte varints and the
  // flag), so a claimed count the payload cannot hold is refused before any
  // allocation sized by it.
  if ((size_t)(end - pos) < (size_t)n * 4) return false;
  idOffset.assign((size_t)n + 1, 0);
  bodyOffset.assign((size_t)n + 1, 0);
  versions.assign(n, 0);
  flags.assign(n, 0);
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!getVarint(pos, end, v) || v > raw.size()) return false;
    idOffset[i + 1] = idOffset[i] + (uint32_t)v;
    if (idOffset[i + 1] < idOffset[i]) return false;
  }
  for (uint32_t i = 0; i < n; i++) {
    if (!getVarint(pos, end, v) || v > raw.size()) return false;
    bodyOffset[i + 1] = bodyOffset[i] + (uint32_t)v;
    if (bodyOffset[i + 1] < bodyOffset[i]) return false;
  }
  for (uint32_t i = 0; i < n; i++) {
    if (!getVarint(pos, end, versions[i])) return false;
  }
  if ((size_t)(end - pos) < n) return false;
  memcpy(flags.data(), pos, n);
  pos += n;
  size_t bytes = (size_t)idOffset[n] + bodyOffset[n];
  if ((size_t)(end - pos) != bytes) return false;
  // Re-base the offsets onto the raw buffer, ids first and bodies after.
  uint32_t idBase = (uint32_t)(pos - raw.data());
  uint32_t bodyBase = idBase + idOffset[n];
  for (auto& o : idOffset) o += idBase;
  for (auto& o : bodyOffset) o += bodyBase;
  return true;
}

Record DecodedBlock::record(uint32_t index) const {
  Record r;
  r.id = std::string_view(raw.data() + idOffset[index], idOffset[index + 1] - idOffset[index]);
  r.body = std::string_view(raw.data() + bodyOffset[index], bodyOffset[index + 1] - bodyOffset[index]);
  r.version = versions[index];
  r.flags = flags[index];
  return r;
}

// ---------------------------------------------------------------------- Codecs

struct Codecs::Impl {
  ZSTD_CCtx* cctx = nullptr;
  ZSTD_DCtx* dctx = nullptr;
  ~Impl() {
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
  }
};

Codecs::Codecs() : impl(std::make_unique<Impl>()) {}
Codecs::~Codecs() = default;

size_t Codecs::compress(Codec codec, int level, const ZSTD_CDict_s* dict, std::string_view src, std::string& dst) {
  if (codec == Codec::NONE) {
    dst.assign(src);
    return dst.size();
  }
  if (codec == Codec::LZ4) {
    if (src.size() > (size_t)LZ4_MAX_INPUT_SIZE) throw std::length_error("source block too large for lz4");
    int bound = LZ4_compressBound((int)src.size());
    dst.resize((size_t)bound);
    int n = LZ4_compress_default(src.data(), dst.data(), (int)src.size(), bound);
    if (n <= 0) throw std::runtime_error("source: lz4 compression failed");
    dst.resize((size_t)n);
    return (size_t)n;
  }
  if (codec != Codec::ZSTD) throw std::invalid_argument("source: unknown codec");
  if (!impl->cctx) {
    impl->cctx = ZSTD_createCCtx();
    if (!impl->cctx) throw std::bad_alloc();
  }
  dst.resize(ZSTD_compressBound(src.size()));
  size_t n = dict ? ZSTD_compress_usingCDict(impl->cctx, dst.data(), dst.size(), src.data(), src.size(), dict)
                  : ZSTD_compressCCtx(impl->cctx, dst.data(), dst.size(), src.data(), src.size(), level);
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("source: zstd compression failed: ") + ZSTD_getErrorName(n));
  dst.resize(n);
  return n;
}

bool Codecs::decompress(Codec codec, const ZSTD_DDict_s* dict, std::string_view src, size_t rawLen, std::string& dst) {
  if (codec == Codec::NONE) {
    if (src.size() != rawLen) return false;
    dst.assign(src);
    return true;
  }
  if (codec == Codec::LZ4) {
    if (src.size() > (size_t)INT32_MAX || rawLen > (size_t)INT32_MAX) return false;
    dst.resize(rawLen);
    int n = LZ4_decompress_safe(src.data(), dst.data(), (int)src.size(), (int)rawLen);
    return n >= 0 && (size_t)n == rawLen;
  }
  if (codec != Codec::ZSTD) return false;
  if (!impl->dctx) {
    impl->dctx = ZSTD_createDCtx();
    if (!impl->dctx) throw std::bad_alloc();
  }
  dst.resize(rawLen);
  size_t n = dict ? ZSTD_decompress_usingDDict(impl->dctx, dst.data(), rawLen, src.data(), src.size(), dict)
                  : ZSTD_decompressDCtx(impl->dctx, dst.data(), rawLen, src.data(), src.size());
  return !ZSTD_isError(n) && n == rawLen;
}

// ------------------------------------------------------------------ FileWriter

namespace {

void encodeHeader(Header& head, std::string& out) {
  out.append(kMagic);
  put32(out, head.format);
  out.append((const char*)head.fileId.bytes.data(), 16);
  out.append((const char*)head.sync.data(), kSyncBytes);
  put64(out, head.createdMs);
  putVarint(out, head.origin.size());
  out.append(head.origin);
  put32(out, lowChecksum(out.data(), out.size()));
  head.size = out.size();
}

}  // namespace

FileWriter::FileWriter(Sink& sink, const WriterOptions& options, std::string_view origin, uint64_t createdMs)
    : sink(sink), options(options) {
  std::random_device rd;
  for (auto& b : head.fileId.bytes) b = (uint8_t)rd();
  for (auto& b : head.sync) b = (uint8_t)rd();
  head.createdMs = createdMs;
  head.origin = std::string(origin);
  std::string bytes;
  encodeHeader(head, bytes);
  emit(bytes.data(), bytes.size());
}

void FileWriter::emit(const char* data, size_t len) {
  sink.write(data, len);
  offset += len;
}

void FileWriter::addDictionary(uint32_t dictId, std::string_view bytes) {
  if (dictId == 0) throw std::invalid_argument("source dictionary id 0 means none");
  seal();
  uint64_t at = offset;
  std::string out;
  out.append((const char*)head.sync.data(), kSyncBytes);
  out.push_back((char)BlockKind::DICT);
  size_t covered = out.size();
  put32(out, dictId);
  put32(out, (uint32_t)bytes.size());
  out.append(bytes);
  put64(out, payloadChecksum(head.fileId, at, out.data() + covered, out.size() - covered));
  emit(out.data(), out.size());
  CompressDict cdict(ZSTD_createCDict(bytes.data(), bytes.size(), options.zstdLevel));
  if (!cdict) throw std::bad_alloc();
  dicts.emplace_back(dictId, std::move(cdict));
  dictOffsets.emplace_back(dictId, at);
}

void FileWriter::useDictionary(uint32_t dictId) {
  seal();
  activeDict = dictId;
  activeDictPtr = nullptr;
  if (dictId == 0) return;
  for (auto& [id, cdict] : dicts) {
    if (id == dictId) {
      activeDictPtr = cdict.get();
      return;
    }
  }
  throw std::invalid_argument("source dictionary " + std::to_string(dictId) + " was not added to this file");
}

void FileWriter::add(const Record& record) {
  if (finished) throw std::logic_error("source file already finished");
  if (record.id.size() > kMaxRawBlock || record.body.size() > kMaxRawBlock) {
    throw std::length_error("source record too large");
  }
  if (idLens.empty()) blockFirst = nextOrdinal;
  idLens.push_back((uint32_t)record.id.size());
  bodyLens.push_back((uint32_t)record.body.size());
  versions.push_back(record.version);
  flags.push_back(record.flags);
  ids.append(record.id);
  bodies.append(record.body);
  nextOrdinal++;
  if (ids.size() + bodies.size() + idLens.size() * 4 >= options.blockBytes ||
      (options.maxDocsPerBlock != 0 && idLens.size() >= options.maxDocsPerBlock)) {
    seal();
  }
}

void FileWriter::seal() {
  if (idLens.empty()) return;
  scratch.clear();
  size_t n = idLens.size();
  putVarint(scratch, n);
  for (auto v : idLens) putVarint(scratch, v);
  for (auto v : bodyLens) putVarint(scratch, v);
  for (auto v : versions) putVarint(scratch, v);
  scratch.append((const char*)flags.data(), n);
  scratch.append(ids);
  scratch.append(bodies);
  if (scratch.size() > kMaxRawBlock) throw std::length_error("source block too large");
  Codec codec = options.codec;
  uint32_t dictId = activeDict;
  size_t comp = codecs.compress(codec, options.zstdLevel, codec == Codec::ZSTD ? activeDictPtr : nullptr, scratch, compressed);
  if (codec != Codec::NONE && comp >= scratch.size()) {
    codec = Codec::NONE;
    dictId = 0;
    compressed = scratch;
  }
  if (codec == Codec::NONE) dictId = 0;
  emitBlock(codec, dictId, blockFirst, (uint32_t)n, (uint32_t)scratch.size(), compressed);
  idLens.clear();
  bodyLens.clear();
  versions.clear();
  flags.clear();
  ids.clear();
  bodies.clear();
}

void FileWriter::emitBlock(Codec codec, uint32_t dictId, uint32_t first, uint32_t nDocs, uint32_t rawLen,
                           std::string_view payload) {
  if (payload.size() > kMaxRawBlock) throw std::length_error("source block too large");
  uint64_t at = offset;
  std::string out;
  out.reserve(kDataHeaderBytes + payload.size() + kChecksumBytes);
  out.append((const char*)head.sync.data(), kSyncBytes);
  size_t covered = out.size();
  out.push_back((char)BlockKind::DATA);
  out.push_back((char)codec);
  put32(out, dictId);
  put32(out, first);
  put32(out, nDocs);
  put32(out, rawLen);
  put32(out, (uint32_t)payload.size());
  put32(out, lowChecksum(out.data() + covered, out.size() - covered));
  out.append(payload);
  put64(out, payloadChecksum(head.fileId, at, payload.data(), payload.size()));
  emit(out.data(), out.size());
  blocks.push_back({first, nDocs, at});
}

void FileWriter::addRawBlock(Codec codec, uint32_t dictId, uint32_t nDocs, uint32_t rawLen, std::string_view payload) {
  if (finished) throw std::logic_error("source file already finished");
  seal();
  emitBlock(codec, dictId, nextOrdinal, nDocs, rawLen, payload);
  nextOrdinal += nDocs;
}

void FileWriter::finish() {
  if (finished) return;
  seal();
  finished = true;
  std::string out;
  for (auto& [id, at] : dictOffsets) {
    put32(out, id);
    put64(out, at);
  }
  for (auto& b : blocks) {
    put32(out, b.firstOrdinal);
    put64(out, b.offset);
  }
  put32(out, (uint32_t)dictOffsets.size());
  put32(out, (uint32_t)blocks.size());
  put32(out, nextOrdinal);
  put64(out, XXH3_64bits(out.data(), out.size()));
  put32(out, (uint32_t)(out.size() + 4 + kEndMagic.size()));
  out.append(kEndMagic);
  emit(out.data(), out.size());
}

// ------------------------------------------------------------------ FileReader

std::optional<Header> FileReader::parseHeader(std::string_view data, std::string* error) {
  auto fail = [&](const char* why) -> std::optional<Header> {
    if (error) *error = why;
    return std::nullopt;
  };
  if (data.size() < kMagic.size() + 4 + 16 + kSyncBytes + 8 + 1 + 4) return fail("source file too short");
  if (data.substr(0, kMagic.size()) != kMagic) return fail("not a source file");
  const char* p = data.data() + kMagic.size();
  const char* end = data.data() + data.size();
  Header h;
  h.format = get32(p);
  p += 4;
  if (h.format != kFormatVersion) return fail("unsupported source file format");
  memcpy(h.fileId.bytes.data(), p, 16);
  p += 16;
  memcpy(h.sync.data(), p, kSyncBytes);
  p += kSyncBytes;
  h.createdMs = get64(p);
  p += 8;
  uint64_t len = 0;
  if (!getVarint(p, end, len) || len > (uint64_t)(end - p) || (uint64_t)(end - p) - len < 4) {
    return fail("bad source file header");
  }
  h.origin.assign(p, (size_t)len);
  p += len;
  size_t covered = (size_t)(p - data.data());
  if (get32(p) != lowChecksum(data.data(), covered)) return fail("source file header checksum mismatch");
  h.size = covered + 4;
  return h;
}

namespace {

// A data block header parsed at `at`; false if it does not verify.
struct DataHeader {
  Codec codec;
  uint32_t dictId, first, nDocs, rawLen, compLen;
  size_t payloadAt;
};

bool parseDataHeader(std::string_view data, uint64_t at, const Header& head, DataHeader& out) {
  if (at > data.size() || data.size() - at < kDataHeaderBytes + kChecksumBytes) return false;
  const char* p = data.data() + at;
  if (memcmp(p, head.sync.data(), kSyncBytes) != 0) return false;
  const char* h = p + kSyncBytes;
  if ((uint8_t)h[0] != (uint8_t)BlockKind::DATA) return false;
  constexpr size_t covered = kDataHeaderBytes - kSyncBytes - 4;
  if (get32(h + covered) != lowChecksum(h, covered)) return false;
  out.codec = (Codec)(uint8_t)h[1];
  out.dictId = get32(h + 2);
  out.first = get32(h + 6);
  out.nDocs = get32(h + 10);
  out.rawLen = get32(h + 14);
  out.compLen = get32(h + 18);
  out.payloadAt = (size_t)at + kDataHeaderBytes;
  if (out.rawLen > kMaxRawBlock || out.nDocs > out.rawLen) return false;
  if ((uint64_t)out.first + out.nDocs > UINT32_MAX) return false;
  if (data.size() - out.payloadAt < (size_t)out.compLen + kChecksumBytes) return false;
  return true;
}

bool verifyDataBlock(std::string_view data, uint64_t at, const Header& head, DataHeader& dh) {
  if (!parseDataHeader(data, at, head, dh)) return false;
  const char* payload = data.data() + dh.payloadAt;
  return get64(payload + dh.compLen) == payloadChecksum(head.fileId, at, payload, dh.compLen);
}

// A dictionary block at `at`; false if it does not verify.
bool readDictBlock(std::string_view data, uint64_t at, const Header& head, uint32_t& dictId, std::string& bytes) {
  constexpr size_t fixed = kSyncBytes + 1 + 4 + 4;
  if (at > data.size() || data.size() - at < fixed + kChecksumBytes) return false;
  const char* p = data.data() + at;
  if (memcmp(p, head.sync.data(), kSyncBytes) != 0 || (uint8_t)p[kSyncBytes] != (uint8_t)BlockKind::DICT) return false;
  const char* body = p + kSyncBytes + 1;
  uint32_t len = get32(body + 4);
  if (len > kMaxRawBlock || data.size() - at - fixed < (size_t)len + kChecksumBytes) return false;
  if (get64(body + 8 + len) != payloadChecksum(head.fileId, at, body, 8 + (size_t)len)) return false;
  dictId = get32(body);
  bytes.assign(body + 8, len);
  return true;
}

}  // namespace

FileReader::ScanResult FileReader::scan(std::string_view data, const Header& head) {
  ScanResult result;
  std::string_view sync((const char*)head.sync.data(), kSyncBytes);
  size_t pos = head.size;
  while (pos + kSyncBytes + 1 <= data.size()) {
    if (data.substr(pos, kSyncBytes) == sync) {
      auto kind = (uint8_t)data[pos + kSyncBytes];
      size_t next = pos + 1;
      if (kind == (uint8_t)BlockKind::DATA) {
        DataHeader dh;
        if (verifyDataBlock(data, pos, head, dh)) {
          result.blocks.push_back({dh.first, dh.nDocs, pos});
          pos = dh.payloadAt + dh.compLen + kChecksumBytes;
          continue;
        }
        result.badBlocks++;
      } else if (kind == (uint8_t)BlockKind::DICT) {
        uint32_t id = 0;
        std::string bytes;
        if (readDictBlock(data, pos, head, id, bytes)) {
          pos += kSyncBytes + 1 + 8 + bytes.size() + kChecksumBytes;
          result.dictionaries.emplace_back(id, std::move(bytes));
          continue;
        }
        result.badBlocks++;
      }
      // Damaged, or a kind this reader does not know: resynchronize.
      pos = next;
    }
    size_t found = data.find(sync, pos);
    if (found == std::string_view::npos) break;
    result.skippedBytes += found - pos;
    pos = found;
  }
  std::ranges::sort(result.blocks, {}, &BlockRef::firstOrdinal);
  // Two verified blocks claiming the same ordinals cannot both be right; the
  // later one in the file is dropped so every ordinal has one block.
  std::vector<BlockRef> disjoint;
  disjoint.reserve(result.blocks.size());
  uint64_t covered = 0;
  for (const auto& block : result.blocks) {
    if (block.nDocs == 0 || block.firstOrdinal < covered) {
      result.badBlocks++;
      continue;
    }
    covered = (uint64_t)block.firstOrdinal + block.nDocs;
    disjoint.push_back(block);
  }
  result.blocks = std::move(disjoint);
  return result;
}

std::optional<FileReader> FileReader::open(std::string_view data, std::string* error) {
  auto head = parseHeader(data, error);
  if (!head) return std::nullopt;
  FileReader reader;
  reader.data = data;
  reader.head = std::move(*head);

  // The footer is an accelerator: any inconsistency falls back to a scan.
  bool footerOk = false;
  if (data.size() >= reader.head.size + 8 + kEndMagic.size() + 4 && data.substr(data.size() - kEndMagic.size()) == kEndMagic) {
    uint32_t footerLen = get32(data.data() + data.size() - kEndMagic.size() - 4);
    constexpr size_t tail = 4 + 4 + 4 + 8 + 4 + 8;  // counts, checksum, length, magic
    if (footerLen >= tail && footerLen <= data.size() - reader.head.size) {
      const char* start = data.data() + data.size() - footerLen;
      const char* counts = data.data() + data.size() - tail;
      uint32_t nDicts = get32(counts);
      uint32_t nBlocks = get32(counts + 4);
      uint32_t docCount = get32(counts + 8);
      uint64_t entries = (uint64_t)nDicts * 12 + (uint64_t)nBlocks * 12;
      if (entries + tail == footerLen &&
          get64(counts + 12) == XXH3_64bits(start, (size_t)(counts + 12 - start))) {
        const char* p = start;
        std::vector<std::pair<uint32_t, uint64_t>> dictAt;
        for (uint32_t i = 0; i < nDicts; i++, p += 12) dictAt.emplace_back(get32(p), get64(p + 4));
        reader.index.reserve(nBlocks);
        for (uint32_t i = 0; i < nBlocks; i++, p += 12) reader.index.push_back({get32(p), 0, get64(p + 4)});
        footerOk = true;
        for (size_t i = 0; i < reader.index.size(); i++) {
          uint32_t last = i + 1 < reader.index.size() ? reader.index[i + 1].firstOrdinal : docCount;
          if (last < reader.index[i].firstOrdinal || (i > 0 && reader.index[i].firstOrdinal <= reader.index[i - 1].firstOrdinal)) {
            footerOk = false;
            break;
          }
          reader.index[i].nDocs = last - reader.index[i].firstOrdinal;
        }
        for (auto [id, at] : dictAt) {
          std::string bytes;
          uint32_t gotId = 0;
          if (!readDictBlock(data, at, reader.head, gotId, bytes) || gotId != id) { footerOk = false; break; }
          reader.dicts.emplace_back(id, std::move(bytes));
        }
        reader.docs = docCount;
      }
    }
  }
  reader.footerOk = footerOk;
  if (!footerOk) {
    reader.index.clear();
    reader.dicts.clear();
    auto scanned = scan(data, reader.head);
    reader.index = std::move(scanned.blocks);
    reader.dicts = std::move(scanned.dictionaries);
    reader.docs = 0;
    for (auto& b : reader.index) reader.docs = std::max(reader.docs, b.firstOrdinal + b.nDocs);
  }
  for (auto& [id, bytes] : reader.dicts) {
    DecompressDict dict(ZSTD_createDDict(bytes.data(), bytes.size()));
    if (!dict) throw std::bad_alloc();
    reader.decompressDicts.emplace_back(id, std::move(dict));
  }
  return reader;
}

int64_t FileReader::blockFor(uint32_t ordinal, int64_t hint) const {
  // Fetches in ordinal order hit the hinted block or the one after it.
  if (hint >= 0) {
    for (int64_t i = hint; i < hint + 2 && i < (int64_t)index.size(); i++) {
      const auto& block = index[(size_t)i];
      if (ordinal >= block.firstOrdinal && ordinal - block.firstOrdinal < block.nDocs) return i;
    }
  }
  auto it = std::ranges::upper_bound(index, ordinal, {}, &BlockRef::firstOrdinal);
  if (it == index.begin()) return -1;
  --it;
  if (ordinal - it->firstOrdinal >= it->nDocs) return -1;
  return it - index.begin();
}

size_t FileWriter::pendingBytes() const {
  return ids.capacity() + bodies.capacity() + idLens.capacity() * sizeof(uint32_t) +
         bodyLens.capacity() * sizeof(uint32_t) + versions.capacity() * sizeof(uint64_t) + flags.capacity() +
         scratch.capacity() + compressed.capacity();
}

const std::string* FileReader::dictionary(uint32_t dictId) const {
  for (auto& [id, bytes] : dicts) {
    if (id == dictId) return &bytes;
  }
  return nullptr;
}

const ZSTD_DDict_s* FileReader::decompressDict(uint32_t dictId) const {
  for (auto& [id, dict] : decompressDicts) {
    if (id == dictId) return dict.get();
  }
  return nullptr;
}

bool FileReader::rawBlock(size_t i, RawBlock& out) const {
  if (i >= index.size()) return false;
  DataHeader dh;
  if (!verifyDataBlock(data, index[i].offset, head, dh)) return false;
  out.codec = dh.codec;
  out.dictId = dh.dictId;
  out.nDocs = dh.nDocs;
  out.rawLen = dh.rawLen;
  out.payload = data.substr(dh.payloadAt, dh.compLen);
  return dh.first == index[i].firstOrdinal && dh.nDocs == index[i].nDocs;
}

bool FileReader::readBlock(size_t i, Codecs& codecs, DecodedBlock& out) const {
  RawBlock raw;
  if (!rawBlock(i, raw)) return false;
  const ZSTD_DDict_s* dict = nullptr;
  if (raw.dictId != 0) {
    dict = decompressDict(raw.dictId);
    if (!dict) return false;
  }
  std::string payload;
  if (!codecs.decompress(raw.codec, dict, raw.payload, raw.rawLen, payload)) return false;
  return out.parse(std::move(payload), index[i].firstOrdinal, raw.nDocs);
}

}  // namespace luxir::source
