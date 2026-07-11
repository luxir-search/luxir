#include "BlockBoundsBuilder.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <unistd.h>
#include <vector>
#include <xxhash.h>

#include "solux/reader/BlockBounds.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/FieldType.h"
#include "solux/search/Similarity.h"
#include "solux/util/MemPool.h"

namespace solux {

namespace {

class CheckedWriter {
  OutputStream out;
  XXH3_state_t checksumState;

  void update(const void* data, size_t len) {
    if (XXH3_64bits_update(&checksumState, data, len) == XXH_ERROR) {
      throw std::runtime_error("BlockBoundsBuilder: checksum update failed");
    }
  }

public:
  explicit CheckedWriter(File* file) : out(file) { XXH3_64bits_reset(&checksumState); }
  uint64_t size() const { return out.size(); }
  void write(const void* data, size_t len) { out.write(data, len); update(data, len); }
  void u32(uint32_t value) { write(&value, sizeof(value)); }
  void u64(uint64_t value) { write(&value, sizeof(value)); }
  void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }
  void finish() {
    uint64_t final = XXH3_64bits_digest(&checksumState);
    out.write(&final, sizeof(final));
    out.close();
  }
};

struct TableEntry {
  uint32_t ord;
  uint64_t offset;
};

float upperDenominator(uint32_t tf, int32_t norm, const std::array<float, 256>& invNorm) {
  if (tf == 0 || tf > (uint32_t) INT32_MAX || norm < 0 || norm > 255) {
    throw std::runtime_error("BlockBoundsBuilder: invalid source impact");
  }
  float d = Similarity::bm25DenominatorUpper(tf, invNorm[(size_t) norm]);
  if (!std::isfinite(d) || !(d >= 1.0f)) {
    throw std::runtime_error("BlockBoundsBuilder: non-finite denominator");
  }
  return d;
}

} // namespace

bool BlockBoundsBuilder::eligible(const SegFieldInfo& fieldInfo) {
  return fieldInfo.type == FieldType::TEXT
      && FieldType::hasFreqs(fieldInfo.flags)
      && FieldType::hasPositions(fieldInfo.flags)
      && fieldInfo.nTerms > 0 && fieldInfo.docsWithField > 0
      && fieldInfo.sumTotalTermFreq > 0;
}

BlockBoundsBuilder::Result BlockBoundsBuilder::build(
    Directory& dir, uint64_t segId, PostingsReader& postingsReader,
    const SegFieldInfo& fieldInfo) {
  auto started = std::chrono::steady_clock::now();
  if (!eligible(fieldInfo)) throw std::runtime_error("BlockBoundsBuilder: ineligible field");

  Similarity similarity;
  Similarity::FieldStats stats;
  stats.maxDoc = postingsReader.maxDoc();
  stats.docsWithField = fieldInfo.docsWithField;
  stats.sumDocFreq = fieldInfo.sumDocFreq;
  stats.sumTotalTermFreq = fieldInfo.sumTotalTermFreq;
  float avgdl = similarity.avgFieldLength(stats);
  float envelope = 2.0f * avgdl;
  if (!std::isfinite(avgdl) || !(avgdl > 0.0f)
      || !std::isfinite(envelope) || !(envelope >= avgdl)) {
    throw std::runtime_error("BlockBoundsBuilder: invalid avgdl envelope");
  }
  std::array<float, 256> invNorm;
  for (int32_t norm = 0; norm < 256; norm++) {
    invNorm[(size_t) norm] = Similarity::bm25InvNorm(
        similarity.k1, similarity.b, SmallFloat::decodeLengthByte((uint8_t) norm), envelope);
  }

  MemPool pool;
  uint32_t admitted = 0;
  uint64_t recordBytes = 0;
  {
    TermsEnum terms(pool, postingsReader, fieldInfo);
    while (terms.nextTerm()) {
      uint32_t blocks = ((uint32_t) terms.docFreq() + Postings::DOCS_BLOCK_SIZE - 1)
          / Postings::DOCS_BLOCK_SIZE;
      if (blocks < 4) continue;
      uint32_t groups = (blocks + DocsEnum::L1_PERIOD - 1) / DocsEnum::L1_PERIOD;
      admitted++;
      recordBytes += BlockBounds::TERM_HEADER_SIZE
          + (uint64_t) (groups + blocks) * BlockBounds::RECORD_SIZE;
    }
  }

  std::string field((std::string_view) fieldInfo.fieldname);
  uint32_t headerSize = BlockBounds::FIXED_HEADER_SIZE + (uint32_t) field.size();
  uint64_t tableOffset = headerSize + recordBytes;
  std::string canonical = BlockBounds::fileName(segId, field);
  uint64_t nonce = (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count();
  std::string unique = std::format("{}.build.{}.{}", canonical, (int32_t) getpid(), nonce);
  auto file = dir.createFile(unique);
  CheckedWriter writer(file.get());
  writer.write(BlockBounds::MAGIC.data(), BlockBounds::MAGIC.size());
  writer.u32(BlockBounds::FORMAT_REVISION);
  writer.u32(headerSize);
  writer.u32(Similarity::SCORING_NORM_REVISION);
  writer.write(Postings::SOLUX_HEADER.data(), Postings::SOLUX_HEADER.size());
  writer.u64(segId);
  writer.u32((uint32_t) fieldInfo.flags);
  writer.u32((uint32_t) fieldInfo.nTerms);
  writer.u32(std::bit_cast<uint32_t>(similarity.k1));
  writer.u32(std::bit_cast<uint32_t>(similarity.b));
  writer.f32(envelope);
  writer.u32((uint32_t) field.size());
  writer.u32(admitted);
  writer.u64(tableOffset);
  writer.u64(tableOffset);
  writer.write(field.data(), field.size());
  if (writer.size() != headerSize) throw std::runtime_error("BlockBoundsBuilder: header size bug");

  std::vector<TableEntry> table;
  table.reserve(admitted);
  uint64_t totalGroups = 0;
  uint64_t totalBlocks = 0;
  TermsEnum terms(pool, postingsReader, fieldInfo);
  DocsEnum::GroupImpacts groups;
  while (terms.nextTerm()) {
    DocsEnum docs(pool, postingsReader, terms);
    uint32_t blocks = (uint32_t) docs.numImpactBlocks();
    if (blocks < 4) continue;
    uint32_t groupCount = (uint32_t) docs.numImpactGroups();
    table.push_back({(uint32_t) docs.termOrd(), writer.size()});
    docs.readGroupImpacts(groups);
    if (groups.lastDocs.size() != groupCount) {
      throw std::runtime_error("BlockBoundsBuilder: group count mismatch");
    }

    float scale = 0.0f;
    docs.visitTermImpactFrontier([&](int32_t tf, int32_t norm) {
      scale = std::max(scale, upperDenominator((uint32_t) tf, norm, invNorm));
    });
    std::vector<float> groupD(groupCount);
    for (uint32_t g = 0; g < groupCount; g++) {
      int32_t begin = groups.frontiers.offsets[g];
      int32_t end = groups.frontiers.offsets[g + 1];
      for (int32_t i = begin; i < end; i++) {
        groupD[g] = std::max(groupD[g], upperDenominator(
            (uint32_t) groups.frontiers.tfs[(size_t) i], groups.frontiers.norms[(size_t) i], invNorm));
      }
      if (groupD[g] == 0.0f) {
        groupD[g] = upperDenominator((uint32_t) groups.spanMaxTfs[g], groups.spanMinNorms[g], invNorm);
      }
      if (groupD[g] > scale) {
        throw std::runtime_error("BlockBoundsBuilder: group denominator exceeds term scale");
      }
    }
    std::vector<int32_t> blockLastDocs;
    std::vector<float> blockD;
    blockLastDocs.reserve(blocks);
    blockD.reserve(blocks);
    DocsEnum::GroupBlockImpactScratch scratch;
    for (uint32_t g = 0; g < groupCount; g++) {
      docs.visitGroupBlockImpacts(
          (int32_t) g, groups.bodyOffsets[g], groups.baseLastDocs[g], scratch,
          [&](int32_t, int32_t lastDoc, int32_t maxTf, int32_t minNorm,
              std::span<const int32_t> tfs, std::span<const int32_t> norms, bool spilled) {
            float d = 0.0f;
            if (spilled) {
              throw std::runtime_error("BlockBoundsBuilder: L0 frontier exceeds reader contract");
            }
            for (size_t i = 0; i < tfs.size(); i++) {
              d = std::max(d, upperDenominator((uint32_t) tfs[i], norms[i], invNorm));
            }
            if (d == 0.0f) d = upperDenominator((uint32_t) maxTf, minNorm, invNorm);
            if (d > scale) {
              throw std::runtime_error("BlockBoundsBuilder: block denominator exceeds term scale");
            }
            blockLastDocs.push_back(lastDoc);
            blockD.push_back(d);
          });
    }
    if (blockD.size() != blocks || !(scale >= 1.0f)) {
      throw std::runtime_error("BlockBoundsBuilder: block count mismatch");
    }
    for (uint32_t g = 0; g < groupCount; g++) {
      uint32_t lastBlock = std::min(blocks, (g + 1) * (uint32_t) DocsEnum::L1_PERIOD) - 1;
      if (groups.lastDocs[g] != blockLastDocs[lastBlock]) {
        throw std::runtime_error("BlockBoundsBuilder: source lastDoc mismatch");
      }
    }

    writer.u32(groupCount);
    writer.u32(blocks);
    writer.f32(scale);
    writer.u32(0);
    for (uint32_t g = 0; g < groupCount; g++) {
      writer.u32((uint32_t) groups.lastDocs[g]);
      writer.f32(groupD[g]);
    }
    for (uint32_t b = 0; b < blocks; b++) {
      writer.u32((uint32_t) blockLastDocs[b]);
      writer.f32(blockD[b]);
    }
    totalGroups += groupCount;
    totalBlocks += blocks;
  }
  if (writer.size() != tableOffset || table.size() != admitted) {
    throw std::runtime_error("BlockBoundsBuilder: table offset bug");
  }
  for (const auto& entry : table) {
    writer.u32(entry.ord);
    writer.u32(0);
    writer.u64(entry.offset);
  }
  writer.finish();
  dir.finishFile(*file);
  std::array<std::string, 2> firstSync{unique, "."};
  dir.sync(firstSync);
  std::string failure;
  if (!BlockBounds::open(dir, segId, fieldInfo, postingsReader.maxDoc(), avgdl,
                         &failure, true, unique)) {
    throw std::runtime_error("BlockBoundsBuilder: unique validation failed: " + failure);
  }
  dir.renameFile(unique, canonical);
  std::array<std::string, 2> finalSync{canonical, "."};
  dir.sync(finalSync);
  auto validated = BlockBounds::open(
      dir, segId, fieldInfo, postingsReader.maxDoc(), avgdl, &failure, true);
  if (!validated) throw std::runtime_error("BlockBoundsBuilder: final validation failed: " + failure);

  Result result;
  result.fileName = canonical;
  result.bytes = (uint64_t) validated->mappedBytes();
  result.admittedTerms = admitted;
  result.groups = totalGroups;
  result.blocks = totalBlocks;
  result.scratchBytes = table.capacity() * sizeof(TableEntry)
      + groups.lastDocs.capacity() * sizeof(int32_t) * 3
      + groups.bodyOffsets.capacity() * sizeof(int64_t)
      + groups.frontiers.tfs.capacity() * sizeof(int32_t) * 2;
  result.avgdl = avgdl;
  result.envelope = envelope;
  result.wallSeconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
  return result;
}

} // namespace solux
