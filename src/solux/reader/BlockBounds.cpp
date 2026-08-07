#include "BlockBounds.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <xxhash.h>

#include "solux/reader/Postings.h"
#include "solux/reader/DocsEnum.h"

namespace solux {

std::atomic<uint64_t> BlockBounds::validationFailures{0};

uint32_t BlockBounds::readU32(const char* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

uint64_t BlockBounds::readU64(const char* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

float BlockBounds::readF32(const char* p) {
  return std::bit_cast<float>(readU32(p));
}

int32_t BlockBounds::TermView::groupCount() const {
  return (int32_t) readU32(record);
}

int32_t BlockBounds::TermView::blockCount() const {
  return (int32_t) readU32(record + 4);
}

float BlockBounds::TermView::denominatorScale() const {
  return readF32(record + 8);
}

int32_t BlockBounds::TermView::groupLastDoc(int32_t group) const {
  assert(group >= 0 && group < groupCount());
  return (int32_t) readU32(record + TERM_HEADER_SIZE + (int64_t) group * RECORD_SIZE);
}

float BlockBounds::TermView::groupDenominator(int32_t group) const {
  assert(group >= 0 && group < groupCount());
  return readF32(record + TERM_HEADER_SIZE + (int64_t) group * RECORD_SIZE + 4);
}

int32_t BlockBounds::TermView::blockLastDoc(int32_t block) const {
  assert(block >= 0 && block < blockCount());
  const char* blocks = record + TERM_HEADER_SIZE + (int64_t) groupCount() * RECORD_SIZE;
  return (int32_t) readU32(blocks + (int64_t) block * RECORD_SIZE);
}

float BlockBounds::TermView::blockDenominator(int32_t block) const {
  assert(block >= 0 && block < blockCount());
  const char* blocks = record + TERM_HEADER_SIZE + (int64_t) groupCount() * RECORD_SIZE;
  return readF32(blocks + (int64_t) block * RECORD_SIZE + 4);
}

int32_t BlockBounds::TermView::groupContaining(int32_t doc) const {
  int32_t lo = 0;
  int32_t hi = groupCount();
  while (lo < hi) {
    int32_t mid = lo + ((hi - lo) >> 1);
    if (groupLastDoc(mid) < doc) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

int32_t BlockBounds::TermView::blockContaining(int32_t doc) const {
  int32_t lo = 0;
  int32_t hi = blockCount();
  while (lo < hi) {
    int32_t mid = lo + ((hi - lo) >> 1);
    if (blockLastDoc(mid) < doc) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

BlockBounds::TermView BlockBounds::find(int32_t termOrd) const {
  uint32_t lo = 0;
  uint32_t hi = admittedTerms;
  while (lo < hi) {
    uint32_t mid = lo + ((hi - lo) >> 1);
    const char* entry = base + tableOffset + (uint64_t) mid * TABLE_ENTRY_SIZE;
    uint32_t ord = readU32(entry);
    if (ord < (uint32_t) termOrd) lo = mid + 1;
    else hi = mid;
  }
  if (lo == admittedTerms) return {};
  const char* entry = base + tableOffset + (uint64_t) lo * TABLE_ENTRY_SIZE;
  if (readU32(entry) != (uint32_t) termOrd) return {};
  return TermView(base + readU64(entry + 8));
}

std::string BlockBounds::fileName(uint64_t segId, std::string_view field) {
  return std::format("{}__bb_{}.bbi", Postings::getIndexFileNamePrefix(segId),
                     Postings::caseSafeName(field));
}

uint64_t BlockBounds::checksum(std::span<const char> bytes) {
  return XXH3_64bits(bytes.data(), bytes.size());
}

std::shared_ptr<BlockBounds> BlockBounds::open(
    Directory& dir, uint64_t segId, const SegFieldInfo& fieldInfo,
    int32_t maxDoc, float globalAvgdl, std::string* failure, bool expectSynced,
    std::string_view fileNameOverride) {
  auto fail = [&](std::string message) -> std::shared_ptr<BlockBounds> {
    validationFailures.fetch_add(1, std::memory_order_relaxed);
    if (failure) *failure = std::move(message);
    return nullptr;
  };
  std::string canonical;
  if (fileNameOverride.empty()) {
    canonical = fileName(segId, (std::string_view) fieldInfo.fieldname);
    fileNameOverride = canonical;
  }
  auto mapped = dir.openFile(fileNameOverride, expectSynced);
  if (!mapped) return nullptr;
  InputStream is = mapped->getInputStream();
  const char* p = is.ptr();
  uint64_t size = (uint64_t) is.size();
  if (size < FIXED_HEADER_SIZE + 4) return fail("file too small");
  if (memcmp(p, MAGIC.data(), MAGIC.size()) != 0) return fail("bad magic");
  if (readU32(p + 8) != FORMAT_REVISION) return fail("bad format revision");
  uint32_t headerSize = readU32(p + 12);
  if (headerSize < FIXED_HEADER_SIZE || headerSize > size - 4) return fail("bad header size");
  if (readU32(p + 16) != Similarity::SCORING_NORM_REVISION) return fail("bad scoring revision");
  if (memcmp(p + 20, Postings::SOLUX_HEADER.data(), Postings::SOLUX_HEADER.size()) != 0) {
    return fail("bad source revision");
  }
  if (readU64(p + 28) != segId) return fail("stale segment id");
  if (readU32(p + 36) != (uint32_t) fieldInfo.flags) return fail("field flags mismatch");
  if (readU32(p + 40) != (uint32_t) fieldInfo.nTerms) return fail("nTerms mismatch");
  Similarity similarity;
  if (readU32(p + 44) != std::bit_cast<uint32_t>(similarity.k1)
      || readU32(p + 48) != std::bit_cast<uint32_t>(similarity.b)) {
    return fail("BM25 signature mismatch");
  }
  float envelope = readF32(p + 52);
  uint32_t fieldLen = readU32(p + 56);
  uint32_t admitted = readU32(p + 60);
  uint64_t tableOff = readU64(p + 64);
  uint64_t dataEnd = readU64(p + 72);
  if ((uint64_t) fieldLen > size - FIXED_HEADER_SIZE - 4
      || (uint64_t) headerSize != (uint64_t) FIXED_HEADER_SIZE + fieldLen) {
    return fail("field header size mismatch");
  }
  std::string_view field(p + FIXED_HEADER_SIZE, fieldLen);
  if (field != (std::string_view) fieldInfo.fieldname) return fail("field mismatch");
  if (!std::isfinite(envelope) || !(envelope > 0.0f)
      || !std::isfinite(globalAvgdl) || !(globalAvgdl > 0.0f)
      || globalAvgdl > envelope) return fail("avgdl outside envelope");
  for (int32_t norm = 0; norm < 256; norm++) {
    float length = SmallFloat::decodeLengthByte((uint8_t) norm);
    float envelopeInv = Similarity::bm25InvNorm(similarity.k1, similarity.b, length, envelope);
    float currentInv = Similarity::bm25InvNorm(similarity.k1, similarity.b, length, globalAvgdl);
    if (!(envelopeInv >= currentInv)) return fail("envelope invNorm does not dominate");
  }
  if (dataEnd != tableOff || tableOff < headerSize || tableOff > size - 4
      || admitted > (size - tableOff - 8) / TABLE_ENTRY_SIZE
      || tableOff + (uint64_t) admitted * TABLE_ENTRY_SIZE + 8 != size) {
    return fail("bad table extent");
  }
  uint64_t storedCrc = readU64(p + size - 8);
  if (checksum({p, (size_t) size - 8}) != storedCrc) return fail("checksum mismatch");

  uint64_t expectedRecord = headerSize;
  uint32_t previousOrd = 0;
  for (uint32_t i = 0; i < admitted; i++) {
    const char* entry = p + tableOff + (uint64_t) i * TABLE_ENTRY_SIZE;
    uint32_t ord = readU32(entry);
    uint64_t off = readU64(entry + 8);
    if (ord >= (uint32_t) fieldInfo.nTerms || (i != 0 && ord <= previousOrd)) {
      return fail("non-monotone term table");
    }
    if (off != expectedRecord || off + TERM_HEADER_SIZE > dataEnd) return fail("bad term offset");
    const char* record = p + off;
    uint32_t groups = readU32(record);
    uint32_t blocks = readU32(record + 4);
    float scale = readF32(record + 8);
    uint64_t expectedGroups = ((uint64_t) blocks + DocsEnumMeta::L1_PERIOD - 1)
        / DocsEnumMeta::L1_PERIOD;
    if (blocks < 4 || groups != expectedGroups
        || !std::isfinite(scale) || !(scale >= 1.0f)) return fail("bad term geometry");
    uint64_t recordEnd = off + TERM_HEADER_SIZE + (uint64_t) (groups + blocks) * RECORD_SIZE;
    if (recordEnd > dataEnd) return fail("term record out of range");
    int32_t prevGroupDoc = -1;
    int32_t prevBlockDoc = -1;
    float maxD = 0.0f;
    for (uint32_t g = 0; g < groups; g++) {
      const char* rec = record + TERM_HEADER_SIZE + (uint64_t) g * RECORD_SIZE;
      int32_t doc = (int32_t) readU32(rec);
      float d = readF32(rec + 4);
      if (doc <= prevGroupDoc || doc < 0 || doc >= maxDoc || !std::isfinite(d) || !(d >= 1.0f)) {
        return fail("bad group record");
      }
      prevGroupDoc = doc;
      maxD = std::max(maxD, d);
    }
    const char* blockBase = record + TERM_HEADER_SIZE + (uint64_t) groups * RECORD_SIZE;
    for (uint32_t b = 0; b < blocks; b++) {
      const char* rec = blockBase + (uint64_t) b * RECORD_SIZE;
      int32_t doc = (int32_t) readU32(rec);
      float d = readF32(rec + 4);
      if (doc <= prevBlockDoc || doc < 0 || doc >= maxDoc || !std::isfinite(d) || !(d >= 1.0f)) {
        return fail("bad block record");
      }
      prevBlockDoc = doc;
      maxD = std::max(maxD, d);
      if ((b + 1) % DocsEnumMeta::L1_PERIOD == 0 || b + 1 == blocks) {
        uint32_t g = b / DocsEnumMeta::L1_PERIOD;
        if ((int32_t) readU32(record + TERM_HEADER_SIZE + (uint64_t) g * RECORD_SIZE) != doc) {
          return fail("group/block lastDoc mismatch");
        }
      }
    }
    if (maxD > scale) return fail("denominator exceeds term scale");
    expectedRecord = recordEnd;
    previousOrd = ord;
  }
  if (expectedRecord != dataEnd) return fail("record extent mismatch");
  return std::make_shared<BlockBounds>(std::move(mapped), p, admitted, tableOff, envelope);
}

} // namespace solux
