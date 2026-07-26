#pragma once

#include "DocsEnum.h"

namespace solux {

// Position ordinals are indexes into the term-global positions list for
// non-pulsed positions. DocsEnumImpl owns doc/freq alignment; PosEnum owns the
// mutable position stream and decoder state.
class PosEnum {
  DocsPosEnum& docsEnum;
  InputStream posIS;
  int32_t* posBuf;
  int64_t posOrd = 0;
  int32_t posBufIdx = 0;
  int32_t posBufEnd = 0;
  int32_t posBufLimit = 0;
  int32_t pos = -1;
  bool positionBatchActive = false;
  int32_t pb[Postings::POSITIONS_BLOCK_SIZE];

  // Returns the ordinal of the final position in the last full encoded block.
  // A term with fewer than one block of positions returns -1.
  static int64_t lastBlockEncodedPosOrd(uint64_t ttf) {
    return (ttf & ~(Postings::POSITIONS_BLOCK_SIZE - 1)) - 1;
  }

  void applyPendingSeek() {
    if (!docsEnum.pendingPosSeekValid) {
      return;
    }

    int64_t candidateOrd = docsEnum.pendingPosBlockOrd;
    int64_t candidateOffset = docsEnum.pendingPosAbsoluteOffset;
    docsEnum.pendingPosSeekValid = false;
    int64_t streamOrd = posOrd + (posBufEnd - posBufIdx);
    if (candidateOrd <= streamOrd) {
      return;
    }

    posIS.seek(candidateOffset);
    posOrd = candidateOrd;
    // A seek invalidates every boundary derived from the old buffer.
    posBufIdx = posBufEnd = posBufLimit = 0;
    skipCount(SkipStats::posSeeks);
  }

  bool ensurePositionDeltaAvailable() {
    if (posBufIdx < posBufLimit) {
      return true;
    }

    int64_t leftToRead = docsEnum.cumulativeTermFreq - posOrd;
    if (leftToRead <= 0) {
      assert(posOrd == docsEnum.cumulativeTermFreq);
      return false;
    }

    if (posBufEnd > posBufLimit) {
      posBufLimit = (int32_t) std::min(
          (int64_t) posBufEnd, (int64_t) posBufIdx + leftToRead);
      return true;
    }

    assert(posBufLimit == posBufEnd);
    if (posOrd <= lastBlockEncodedPosOrd(docsEnum.ttf)) {
      uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
      auto bytesRead = IndexCodec::posCodec.decodeBlock(
          posIS.ptr(), posIS.left(), (uint32_t*) posBuf, outSz);
      posIS.skip(bytesRead);
      assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
      skipCount(SkipStats::posBlocksDecoded);
      posBufIdx = 0;
      posBufEnd = (int32_t) outSz;
      posBufLimit = (int32_t) std::min((int64_t) posBufEnd, leftToRead);
    } else {
      assert(leftToRead < Postings::POSITIONS_BLOCK_SIZE);
      posBufIdx = 0;
      posBufEnd = (int32_t) leftToRead;
      posBufLimit = posBufEnd;
      for (int32_t i = posBufIdx; i < posBufEnd; i++) {
        posBuf[i] = (int32_t) posIS.readVint();
      }
    }
    return true;
  }

  void finishPositionBatch() {
    positionBatchActive = false;
    docsEnum.positionBatchActive = false;
  }

public:
  static constexpr int32_t END = PostingsReader::END;

  explicit PosEnum(DocsPosEnum& docsEnum)
      : docsEnum(docsEnum), posIS(docsEnum.positionInput), posBuf(pb) {
    assert(docsEnum.hasPositions);
    assert(!docsEnum.positionTrackingEnabled);
    assert(docsEnum.docid == -1);
    assert(docsEnum.docOrd == 0);
    assert(docsEnum.docBufIdx == 0);
    assert(!docsEnum.blockMode);
    assert(!docsEnum.docsOnlyConsumed);
    assert(!docsEnum.docBlockResident);
    assert(!docsEnum.scoredProbeActive);
    assert(!docsEnum.positionBatchActive);
    docsEnum.enablePositionTracking();

    if (docsEnum.docsSize == 0) {
      posBuf[0] = docsEnum.pulsedPosition;
      posBufEnd = posBufLimit = 1;
    } else {
      assert(posIS.offset() == docsEnum.posStartLoc);
    }
  }

  PosEnum(const PosEnum&) = delete;

  void startPositions() {
    assert(!docsEnum.docBlockResident);
    assert(!docsEnum.docsOnlyConsumed);
    assert(!docsEnum.scoredProbeActive);
    assert(!docsEnum.positionBatchActive);
    assert(!positionBatchActive);
    assert(docsEnum.positionTrackingEnabled);
    docsEnum.materializePendingPositionRepair();
    applyPendingSeek();
    pos = -1;
    while (posOrd < docsEnum.posOrdStart) {
      auto numToSkip = docsEnum.posOrdStart - posOrd;
      auto leftInBlock = posBufEnd - posBufIdx;

      if (numToSkip <= leftInBlock) {
        posBufIdx += (int32_t) numToSkip;
        posOrd += numToSkip;
        posBufLimit = std::min(posBufIdx + docsEnum.tfreq, posBufEnd);
        return;
      }

      posOrd += leftInBlock;
      numToSkip -= leftInBlock;
      posBufIdx = posBufEnd;

      if (numToSkip >= Postings::POSITIONS_BLOCK_SIZE) {
        auto bytesSkipped = IndexCodec::posCodec.skipBlock(posIS.ptr(), posIS.left());
        posIS.skip(bytesSkipped);
        posOrd += Postings::POSITIONS_BLOCK_SIZE;
        skipCount(SkipStats::posBlocksSkipped);
        posBufIdx = posBufEnd = posBufLimit = 0;
        continue;
      }

      if (docsEnum.posOrdStart > lastBlockEncodedPosOrd(docsEnum.ttf)) {
        for (int64_t i = 0; i < numToSkip; i++) {
          auto delta = posIS.readVint();
          unused(delta);
        }
        posOrd += numToSkip;
        assert(posOrd == docsEnum.posOrdStart);
        posBufIdx = posBufEnd = posBufLimit = 0;
        return;
      }

      uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
      auto bytesRead = IndexCodec::posCodec.decodeBlock(
          posIS.ptr(), posIS.left(), (uint32_t*) posBuf, outSz);
      posIS.skip(bytesRead);
      assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
      skipCount(SkipStats::posBlocksDecoded);
      posBufIdx = 0;
      posBufEnd = (int32_t) outSz;
      posBufLimit = std::min(posBufEnd, docsEnum.tfreq);
    }
  }

  int32_t nextPosition() {
    assert(!docsEnum.docBlockResident);
    assert(!docsEnum.scoredProbeActive);
    assert(!docsEnum.positionBatchActive);
    assert(!positionBatchActive);
    if (!ensurePositionDeltaAvailable()) {
      pos = END;
      return pos;
    }
    auto delta = posBuf[posBufIdx++];
    posOrd++;
    pos += delta;
    return pos;
  }

  std::span<const int32_t> nextPositionDeltaSpan() {
    assert(!docsEnum.docBlockResident);
    assert(!docsEnum.scoredProbeActive);
    assert(!docsEnum.positionBatchActive);
    assert(!positionBatchActive);
    if (!ensurePositionDeltaAvailable()) {
      return {};
    }
    int32_t start = posBufIdx;
    posBufIdx = posBufLimit;
    posOrd += posBufLimit - start;
    return {posBuf + start, (size_t) (posBufLimit - start)};
  }

  int32_t advancePosition(int32_t target) {
    assert(!docsEnum.docBlockResident);
    assert(!docsEnum.scoredProbeActive);
    while (pos < target) {
      nextPosition();
    }
    return pos;
  }

  void beginPositionDeltaBatch(int32_t docCount) {
    assert(!positionBatchActive);
    startPositions();
    int64_t batchEndOrd = docsEnum.beginPositionDeltaBatchDocs(docCount);
    posBufLimit = (int32_t) std::min(
        (int64_t) posBufEnd, (int64_t) posBufIdx + batchEndOrd - posOrd);
    assert(batchEndOrd > posOrd);
    positionBatchActive = true;
  }

  std::span<const int32_t> nextPositionDeltaBatchSpan(int64_t maxCount) {
    assert(positionBatchActive);
    assert(docsEnum.positionBatchActive);
    assert(maxCount > 0);
    if (!ensurePositionDeltaAvailable()) {
      finishPositionBatch();
      return {};
    }
    int32_t start = posBufIdx;
    int64_t available = posBufLimit - posBufIdx;
    int32_t count = (int32_t) std::min(available, maxCount);
    posBufIdx += count;
    posOrd += count;
    assert(posOrd <= docsEnum.cumulativeTermFreq);
    if (posOrd == docsEnum.cumulativeTermFreq) {
      finishPositionBatch();
    }
    return {posBuf + start, (size_t) count};
  }
};

}
