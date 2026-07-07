#pragma once

#include "TermsEnum.h"
#include "SkipStats.h"
#include "solux/codec/Codec.h"
#include "solux/codec/StreamVByte.h"
#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace solux {
// TODO: templatize to be able to instrument, implement checkindex, etc...
// TODO: investigate writing a version of this based on continuations and see how it performs?
// TODO: some of this internal state could be removed... we only need some of it in the constructor?
// Implementation note: moving block reading of docs and positions to cpp files and just leaving the hot path
// in the header resulted in >3% performance loss for docs, and >5% loss for positions.
class DocsEnum {
  // Position ordinals are indexes into the term-global positions list for non-pulsed positions.
  // Thus the max posOrd should thus be totalTermFreq (except in the case of a single pulsed term,
  // in which case there is nothing to read from the posFile anyway).
  int32_t* posBuf;       // the list of decoded position deltas (may be partial)
  int32_t* docBuf;       // the list of decoded docs (may be partial)
  int32_t* tfreqBuf;     // the list of decoded term freqs

  int64_t posOrd = 0;         // the ordinal of the current position we are on
  int64_t posOrdStart = 0;    // the starting position ordinal for the current doc
  int64_t cumulativeTermFreq; // Synonym for posOrdEnd.  Should be equal to posOrdStart + tfreq (i.e. a docs positions are [posOrdStart,cumulativeTermFreq)

  int32_t posBufIdx = 0; // index of the next value to read in the position buffer
  int32_t posBufEnd;     // one-past the last decoded position delta (but may be past the deltas for *this* doc
  int32_t posBufEndDoc;  // one-past the last decoded pos delta for *this* doc
  int32_t pos;           // current position

  int32_t docOrd = 0;    // the ordinal of the current document we are on for this term
  int32_t docBufIdx = 0; // index of the next value to read in the docs buffer
  int32_t docBufEnd;     // index of one-past the last valid element
  int32_t docid;         // current docid
  bool blockMode = false;

  // For term freqs, since they are parallel to docs, we don't actually need
  // all of these variables.  But it sets the stage for separating the two
  // more and using different encodings / block sizes for them.
  int32_t tfreqOrd = 0;      // ordinal of the current term freq (parallel to docOrd)
  int32_t tfreqBufIdx = 0;   // index of the next valid value
  int32_t tfreqBufEnd;       // index of one-past the last valid element
  int32_t tfreq;


  InputStream docIS;
  InputStream posIS;
  PostingsReader& postingsReader;
  const SegFieldInfo& fieldInfo;
  MemPool* pool;
  bool hasFreqs;      // field indexes term freqs (else tfreq is implicitly 1)
  bool hasPositions;  // field indexes positions (else there is no position stream)
  bool trackPositions; // keep cumulative-tf/position state current for startPositions()
  bool hasNorms;      // text fields with positions carry encoded length norms
  int32_t docfreq; // number of docs containing this term
  int64_t ttf;    // totalTermFreq (sum of term freq across all docs for this term)

  int64_t docsSize;  // size of the postings in the doc file for the given term
  int64_t startOfDocs;
  int64_t endOfDocs;

  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTermBlock;
  int64_t posStartLoc = 0;  // absolute location of this term's positions (base for L0 posByteOff)

public:
  // The skip structure is two levels: L0 per-block headers, and L1 group headers
  // every L1_PERIOD blocks (see skipToBlock). There is deliberately NO level 2
  // (a coarser index above L1). Measured 2026-07-05 with the skip-effectiveness
  // harness (BM_SkipEffectiveness) at 1M docs: even in the aggressive-skip regime
  // (common + rare high-idf disjunction, ~15% of blocks decoded) the L1 group
  // walk stays tiny (~100 group-header reads for the whole query), so an L2 above
  // it would save almost nothing. The header cost that actually dominates there is
  // the WITHIN-group L0 walk (up to L1_PERIOD headers to reach the target block),
  // which an L2 does not touch - the levers for that would be L1_PERIOD or a
  // within-group L0 jump, not another level. And it is not gating regardless: the
  // L0 walk is cheap vint reads, so skipping is a clear wall-clock win as-is.
  // Revisit only if l0HeaderSteps/blocksDecoded blows up on much longer lists
  // (8M+ docs); measure with the harness before adding structure.
  static constexpr int32_t L1_PERIOD = 32;
  static constexpr int32_t L1_DOCS = L1_PERIOD * Postings::DOCS_BLOCK_SIZE;

  struct ImpactFrontiers {
    std::vector<int32_t> offsets;
    std::vector<int32_t> tfs;
    std::vector<int32_t> norms;

    void clear() {
      offsets.resize(0);
      tfs.resize(0);
      norms.resize(0);
    }

    int32_t count(int32_t block) const {
      assert(block >= 0);
      assert(block + 1 < (int32_t) offsets.size());
      return offsets[(size_t) block + 1] - offsets[(size_t) block];
    }
  };

  struct GroupBlockImpactScratch {
    static constexpr int32_t FRONTIER_CAP = 256;
    int32_t frontierTfs[FRONTIER_CAP];
    int32_t frontierNorms[FRONTIER_CAP];
  };

private:

  int32_t numDocBlocks = 0;
  int32_t numDocGroups = 0;
  int32_t nextL0Block = 0;     // block whose header is at docIS, unless bodyReady is true
  uint32_t nextL0Base = 0;     // last doc before nextL0Block
  int64_t nextL0CumTf = 0;     // cumulative tf before nextL0Block
  int32_t nextL1Group = 0;     // group whose header is next when nextL0Block is on a group boundary
  uint32_t nextL1Base = 0;     // last doc before nextL1Group
  int64_t nextL1CumTf = 0;     // cumulative tf before nextL1Group
  bool bodyReady = false;      // docIS already points at the current block body after an L0 walk

  int32_t db[Postings::DOCS_BLOCK_SIZE];  // temporary...
  int32_t pb[Postings::POSITIONS_BLOCK_SIZE];
  int32_t tb[Postings::POSITIONS_BLOCK_SIZE];

  // returns the ord of the last position in the last full block.. i.e. for 150 positions, it would return 127 (0-127 are in first block)
  // for 10, it would return -1 (there are no block encoded positions)
  static int64_t lastBlockEncodedPosOrd(uint64_t ttf) {
    // A ttf of 127 means we don't have a full block... so return -1.
    // A ttf of 128 through (128+127) means ords 0-127 are in first block and we would want to return 127.
    return (ttf & ~(Postings::POSITIONS_BLOCK_SIZE-1)) - 1;
  }

  static uint32_t readVint15(const char*& pos, const char* end) {
    assert(pos + 2 <= end);
    uint16_t s = (uint16_t) (uint8_t) pos[0] | (uint16_t) ((uint16_t) (uint8_t) pos[1] << 8);
    pos += 2;
    if ((s & 0x8000u) == 0) {
      return s;
    }
    return (s & 0x7fffu) | (InputStream::readVint(pos, end) << 15);
  }

  static uint64_t readVlong15(const char*& pos, const char* end) {
    assert(pos + 2 <= end);
    uint16_t s = (uint16_t) (uint8_t) pos[0] | (uint16_t) ((uint16_t) (uint8_t) pos[1] << 8);
    pos += 2;
    if ((s & 0x8000u) == 0) {
      return s;
    }
    return (s & 0x7fffull) | (InputStream::readVlong(pos, end) << 15);
  }

  static bool isL1Boundary(int32_t block) {
    return (block % L1_PERIOD) == 0;
  }

public:
  /// sentinel value used for both docs and positions
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // After this constructor has finished, this DocsEnum instance is independent of the TermsEnum instance.
  // This instance *does* rely on fieldInfo that was passed into the TermsEnum instance still being valid.
  DocsEnum(MemPool& pool, PostingsReader& postingsReader, TermsEnum& tenum,
           int32_t* docsScratch=nullptr, int32_t* posScratch=nullptr, int32_t* tfreqScratch=nullptr)
  : postingsReader(postingsReader), fieldInfo(tenum.fieldInfo), pool(&pool)
  {
    unused(docsScratch, posScratch, tfreqScratch);
    docBuf=db;
    posBuf=pb;
    tfreqBuf=tb;
    hasFreqs = FieldType::hasFreqs(fieldInfo.flags);
    hasPositions = FieldType::hasPositions(fieldInfo.flags);
    trackPositions = hasPositions;
    hasNorms = hasPositions;

    // Since the same terms enum will often be used for multiple docs enum, we should copy everything we need
    // from the terms enum that we need (that may change.)
    // TODO: package those dependencies in a struct that can be simply assigned?  Or if there is enough overlap, simply copy the complete tenum?
    // Or we could invert the responsibility and make the client copy the tenum if they are going to change it.
    // Term-level postings bounds and stats are handed over by TermsEnum from
    // the term-block metadata section.  The docs stream has no per-term trailer:
    // DocsEnum receives absolute docsStart/docsEnd, df/ttf, posOffset, and any
    // pulsed doc/pos payload through these accessors before it starts reading
    // doc blocks or impact headers.
    docsSize = tenum.docsSize();
    docid = -1;

    // TODO: look into deferring filling the buffer until we need it, then we can avoid allocating the buffers for a shared DocsEnum.
    if (docsSize == 0) {
      // postings pulsed
      docfreq = tenum.docFreq();
      assert(docfreq == 1);
      tfreq = 1;
      ttf = tenum.totalTermFreq();
      assert(ttf == 1);
      // fill buffers with the single pulsed doc (+ position, if the field indexes them)
      docBuf[0] = tenum.pulsedDoc();
      docBufEnd = 1;
      tfreqBuf[0] = 1;
      tfreqBufEnd = 1;
      posBufIdx = 0;
      if (hasPositions) {
        posBuf[0] = tenum.pulsedPos();
        posBufEndDoc = posBufEnd = 1;
      } else {
        posBufEndDoc = posBufEnd = 0;
      }
      cumulativeTermFreq = 0;  // this will be incremented in nextDoc()
      // std::cout << "Pulsed posting: id=" << docid << "pos=" << posBuf[0] << std::endl;

    } else {
      pos = tfreq = -1;  // unnecessary initializations, but it makes some maybe-uninitialized warnings go away with -O3  // todo: revisit
      locOfDocsForTermBlock = tenum.locOfDocsForTermBlock;
      locOfPositionsForTermBlock = tenum.locOfPositionsForTermBlock;
      docIS = postingsReader.getInputStream(fieldInfo.docsLoc.filenum());
      startOfDocs = tenum.docsStart();
      endOfDocs = tenum.docsEnd();
      assert(endOfDocs >= startOfDocs);
      assert(endOfDocs - startOfDocs == docsSize);
      docfreq = tenum.docFreq();
      ttf = tenum.totalTermFreq();
      docBufEnd = 0;
      numDocBlocks = (docfreq + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
      numDocGroups = (numDocBlocks + L1_PERIOD - 1) / L1_PERIOD;

      if (hasPositions) {
        auto posOffset = tenum.posOffset();
        posIS = postingsReader.getInputStream(fieldInfo.posLoc.filenum());
        posStartLoc = locOfPositionsForTermBlock + posOffset;
        posIS.seek(posStartLoc);
      }

      docIS.seek(startOfDocs);

      posBufEndDoc = posBufEnd = 0; // no positions read yet
      cumulativeTermFreq = 0;
      nextL0Block = 0;
      nextL0Base = 0;
      nextL0CumTf = 0;
      nextL1Group = 0;
      nextL1Base = 0;
      nextL1CumTf = 0;
      bodyReady = false;
      // std::cout << "Normal posting" << std::endl;
    }
  }

  DocsEnum(const DocsEnum& other) = delete;

  DocsEnum(MemPool& pool, const DocsEnum& other) : postingsReader(other.postingsReader), fieldInfo(other.fieldInfo) {
    memcpy(this, &other, sizeof(DocsEnum));  // is there a better way to copy everything that can be copied so we don't forget anything?
    // re-point the internal pointers
    this->pool = &pool; // new pool (currently unused though since buffers are immediate)
    docBuf = db;
    posBuf = pb;
    tfreqBuf = tb;
    // If buffers cease to be immediate, we need to copy in the pulsed docs/positions/freqs (first element)
  }

  /// whether this field indexes positions (false for DOCS / DOCS_AND_FREQS fields)
  bool indexHasPositions() const {
    return hasPositions;
  }

  void setTrackPositions(bool enabled) {
    trackPositions = enabled && hasPositions;
  }

  /// number of documents containing the term
  int32_t numDocs() {
    return docfreq;
  }

  /// sum of term freq across all documents (i.e. total number of appearances for this term)
  int32_t totalTermFreq() {
    return ttf;
  }

  /// the document this iterator is currently positions on
  int32_t docId() {
    return docid;
  }

  /// number of times the term appears in the current document
  int32_t termFreq() {
    return tfreq;
  }

  // we also have a next() to align with scorers
  int32_t next() {
    return nextDoc();
  }

  int32_t nextDoc() {
    // Contract: callers must not re-poll after END (see Query::Scorer).
    assert(docid != PostingsReader::END);
    blockMode = false;
    if (docBufIdx >= docBufEnd) {
      auto leftToRead = docfreq - docOrd;
      // Boundary analysis: if docfreq==1 and docOrd==1 (meaning we already read ord 0, but not 1), we are done.
      if (leftToRead <= 0) {
        assert(leftToRead == 0);
        docid = PostingsReader::END;
        return docid;
      }

      // Cross-block delta base: the previous block's last id (still in docBuf,
      // not yet overwritten), or 0 for the first block.  Mirrors PostingsWriter
      // (full blocks via the docs codec, the partial tail via StreamVByte d1).
      const uint32_t base = (docOrd == 0) ? 0 : (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
      const int32_t blockStartOrd = docOrd;
      const int64_t blockStartCumTf = cumulativeTermFreq;

      if (!bodyReady) {
        if (isL1Boundary(nextL0Block)) {
          auto groupHeaderLen = docIS.readVint();
          docIS.skip(groupHeaderLen);
        }
        auto headerLen = docIS.readVint();
        docIS.skip(headerLen);
      } else {
        bodyReady = false;
      }
      // Every path from here decodes exactly one docs block (full block or tail).
      skipCount(SkipStats::docBlocksDecoded);

      // Since we only read whole blocks, simply comparing with number of docs left to read is sufficient.
      // If we start partial decoding of blocks (say because of skipping), then we would want something
      // like lastBlockEncodedPosOrd, but for docs.
      if (leftToRead >= Postings::DOCS_BLOCK_SIZE) {
        uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
        auto bytesRead = IndexCodec::docCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)docBuf, outSz, base);
        docIS.skip(bytesRead);
        assert(outSz == Postings::DOCS_BLOCK_SIZE);
        docBufIdx = 0;
        docBufEnd = Postings::DOCS_BLOCK_SIZE;
        // std::cout << "read doc block: " << std::endl;

        // Term freqs are omitted entirely for DOCS-only fields; tfreq is then implicitly 1.
        // TODO: we should really decode term freqs lazily in case they aren't needed... but this is far simpler for now.
        if (hasFreqs) {
          outSz = Postings::DOCS_BLOCK_SIZE;  // currently parallel to docs, so must be same block size
          bytesRead = IndexCodec::tfreqCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)tfreqBuf, outSz);
          docIS.skip(bytesRead);
          assert(outSz == Postings::DOCS_BLOCK_SIZE);
          tfreqBufIdx = 0;
          tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
        }
        // std::cout << "read tfreq block: " << std::endl;
        if (trackPositions) {
          int64_t blockTfSum = 0;
          for (int i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
            blockTfSum += tfreqBuf[i];
          }
          nextL0CumTf = blockStartCumTf + blockTfSum;
        }
        nextL0Base = (uint32_t) docBuf[Postings::DOCS_BLOCK_SIZE - 1];
        nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
        if (isL1Boundary(nextL0Block)) {
          nextL1Group = nextL0Block / L1_PERIOD;
          nextL1Base = nextL0Base;
          nextL1CumTf = nextL0CumTf;
        }
      } else {
        // StreamVByte tail layout written by PostingsWriter::endTerm:
        //   [docKeys][docData] followed, for fields with freqs, by [tfreqKeys][tfreqData].
        // Docs are d1 decoded; freqs are plain StreamVByte values. The AVX decoders
        // may read up to SVB_OVERREAD_PAD bytes past the encoded data, covered by
        // the trailing padding PostingsWriter::finish() reserves on the docs file.
        const uint32_t n = (uint32_t) leftToRead;
        const uint32_t kb = svbKeyBytes(n);
        uint8_t* p = (uint8_t*) docIS.ptr();
        uint8_t* dataEnd = svb_decode_avx_d1_init((uint32_t*) docBuf, p, p + kb, n, base);
        if (hasFreqs) {
          dataEnd = svb_decode_avx_simple((uint32_t*) tfreqBuf, dataEnd, dataEnd + kb, n);
          tfreqBufIdx = 0;
          tfreqBufEnd = leftToRead;
        }
        docIS.skip(dataEnd - p);

        docBufIdx = 0;
        docBufEnd = leftToRead;
        if (trackPositions) {
          int64_t blockTfSum = 0;
          for (int i = 0; i < leftToRead; i++) {
            blockTfSum += tfreqBuf[i];
          }
          nextL0CumTf = blockStartCumTf + blockTfSum;
        }
        nextL0Base = (uint32_t) docBuf[leftToRead - 1];
        nextL0Block = blockStartOrd / Postings::DOCS_BLOCK_SIZE + 1;
        if (isL1Boundary(nextL0Block)) {
          nextL1Group = nextL0Block / L1_PERIOD;
          nextL1Base = nextL0Base;
          nextL1CumTf = nextL0CumTf;
        }

      } // end decode tail
    }

    // Keep tfreq current for scoring; cumulativeTermFreq is only needed by
    // position consumers such as PhraseQuery.
    assert(!hasFreqs || docBufIdx == tfreqBufIdx);
    assert(!hasFreqs || docOrd == tfreqOrd);

    docid = docBuf[docBufIdx++];
    docOrd++;

    // DOCS-only fields have no freq stream; tfreq is implicitly 1.
    if (hasFreqs) {
      tfreq = tfreqBuf[tfreqBufIdx++];
      tfreqOrd++;
    } else {
      tfreq = 1;
    }
    if (trackPositions) {
      posOrdStart = cumulativeTermFreq;
      cumulativeTermFreq += tfreq;
    }

    return docid;
  }

  // Return remaining decoded docs/freqs from the current block. This is a
  // peek-only block-mode API: consumeDocFreqBlock() is the only cursor mutation
  // past the returned span. It may call nextDoc() to decode the next block, and
  // therefore may position docId()/termFreq() at the first returned doc. It does
  // not keep position/cumulative-tf metadata synchronized because score-only
  // block consumers do not need it. Spans are valid until the next DocsEnum call.
  // Impact threshold ownership remains with TermQuery::Scorer; this exposes raw
  // decoded postings and does not know minCompetitiveScore.
  std::pair<std::span<const int32_t>, std::span<const int32_t>> peekDocFreqBlock() {
    if (docid == PostingsReader::END) {
      return {};
    }

    int32_t start = 0;
    if (!blockMode) {
      if (docid < 0) {
        int32_t doc = nextDoc();
        if (doc == PostingsReader::END) {
          return {};
        }
      }
      start = docBufIdx - 1;
    } else {
      if (docBufIdx >= docBufEnd) {
        int32_t doc = nextDoc();
        if (doc == PostingsReader::END) {
          return {};
        }
        start = docBufIdx - 1;
      } else {
        start = docBufIdx;
      }
    }

    if (start >= docBufEnd) {
      return {};
    }

    if (!hasFreqs) {
      std::fill(tfreqBuf + start, tfreqBuf + docBufEnd, 1);
    }

    int32_t emitted = docBufEnd - start;
    return {
      std::span<const int32_t>(docBuf + start, (size_t) emitted),
      std::span<const int32_t>(tfreqBuf + start, (size_t) emitted)
    };
  }

  std::span<const int32_t> peekDocBlock() {
    return peekDocFreqBlock().first;
  }

  void consumeDocFreqBlock(int32_t n) {
    assert(n >= 0);
    if (n == 0) {
      return;
    }
    assert(docid != PostingsReader::END);

    int32_t start = 0;
    bool countedCurrent = false;
    if (!blockMode) {
      assert(docid >= 0);
      start = docBufIdx - 1;
      countedCurrent = true;
    } else {
      start = docBufIdx;
    }

    int32_t limit = start + n;
    assert(start >= 0);
    assert(limit <= docBufEnd);

    int32_t newlyCounted = n - (countedCurrent ? 1 : 0);
    assert(newlyCounted >= 0);
    docOrd += newlyCounted;
    if (hasFreqs) {
      tfreqOrd += newlyCounted;
    }
    docBufIdx = limit;
    if (hasFreqs) {
      tfreqBufIdx = limit;
      tfreq = tfreqBuf[limit - 1];
    } else {
      tfreq = 1;
    }
    docid = docBuf[limit - 1];
    blockMode = true;
  }


  // Reset the doc decoder to the block that may contain target.  Position state is
  // repaired by cumulativeTermFreq; the position stream itself stays lazy.
  void skipToBlock(int32_t target) {
    blockMode = false;
    const char* const streamStart = docIS.ptr(0);
    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr();
    int32_t block = nextL0Block;
    uint32_t prevLastDoc = nextL0Base;
    int64_t cumTf = nextL0CumTf;

    auto walkL0To = [&](int32_t maxBlock) -> bool {
      while (block < maxBlock && block < numDocBlocks) {
        skipCount(SkipStats::l0HeaderSteps);
        uint32_t headerLen = InputStream::readVint(p, end);
        const char* headerEnd = p + headerLen;
        assert(headerEnd <= end);
        uint32_t lastDocDelta = readVint15(p, headerEnd);
        uint32_t blockLastDoc = prevLastDoc + lastDocDelta;
        uint64_t blockByteLen = readVlong15(p, headerEnd);
        int32_t blockDocCount = std::min(Postings::DOCS_BLOCK_SIZE,
                                         docfreq - block * Postings::DOCS_BLOCK_SIZE);
        int64_t blockTfSum = blockDocCount;
        uint64_t blockPosByteOff = 0;
        if (hasPositions) {
          blockTfSum += InputStream::readVint(p, headerEnd);
          blockPosByteOff = InputStream::readVlong(p, headerEnd);
        }
        if (hasFreqs && hasNorms) {
          uint32_t frontierCount = InputStream::readVint(p, headerEnd);
          for (uint32_t i = 0; i < frontierCount; i++) {
            assert(p < headerEnd);
            p++;  // norm: one raw byte
            auto tfDelta = InputStream::readVint(p, headerEnd);
            unused(tfDelta);
          }
        } else if (hasFreqs) {
          auto maxTf = InputStream::readVint(p, headerEnd);
          unused(maxTf);
        } else if (hasNorms) {
          auto minNorm = InputStream::readVint(p, headerEnd);
          unused(minNorm);
        }
        assert(p == headerEnd);

        const char* body = headerEnd;
        if (target <= (int32_t) blockLastDoc) {
          docIS.seek(body - streamStart);
          docOrd = block * Postings::DOCS_BLOCK_SIZE;
          tfreqOrd = docOrd;
          cumulativeTermFreq = cumTf;  // restores position alignment; unused when !trackPositions
          if (trackPositions) {
            // This doc block's first position is cumTf % POSITIONS_BLOCK_SIZE
            // positions into the position block at posByteOff.  Seek there
            // directly instead of letting startPositions walk the intervening
            // blocks - unless the stream already sits at or past that block
            // (then the buffered state is still the cheapest path forward).
            int64_t posBlockStartOrd = cumTf - (cumTf % Postings::POSITIONS_BLOCK_SIZE);
            int64_t streamOrd = posOrd + (posBufEnd - posBufIdx);
            if (posBlockStartOrd > streamOrd) {
              posIS.seek(posStartLoc + (int64_t) blockPosByteOff);
              posOrd = posBlockStartOrd;
              // Full buffer reset (stale posBufEndDoc reads past posBuf - see
              // the startPositions whole-block skip note).
              posBufIdx = posBufEnd = posBufEndDoc = 0;
              skipCount(SkipStats::posSeeks);
            }
          }
          docBuf[Postings::DOCS_BLOCK_SIZE - 1] = (int32_t) prevLastDoc;
          docBufIdx = docBufEnd = Postings::DOCS_BLOCK_SIZE;   // force a decode on the next nextDoc()
          tfreqBufIdx = tfreqBufEnd = 0;
          nextL0Block = block;
          nextL0Base = prevLastDoc;
          nextL0CumTf = cumTf;
          if (isL1Boundary(block)) {
            nextL1Group = block / L1_PERIOD;
            nextL1Base = prevLastDoc;
            nextL1CumTf = cumTf;
          }
          bodyReady = true;
          return true;
        }

        p = body + (int64_t) blockByteLen;
        assert(p <= end);
        prevLastDoc = blockLastDoc;
        cumTf += blockTfSum;
        block++;
      }
      return false;
    };

    if (!isL1Boundary(block)) {
      int32_t nextGroupBlock = ((block / L1_PERIOD) + 1) * L1_PERIOD;
      if (walkL0To(nextGroupBlock)) {
        return;
      }
    }

    while (block < numDocBlocks) {
      assert(isL1Boundary(block));
      skipCount(SkipStats::l1GroupSteps);
      int32_t group = block / L1_PERIOD;
      nextL1Group = group;
      nextL1Base = prevLastDoc;
      nextL1CumTf = cumTf;

      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDocDelta = readVint15(p, groupHeaderEnd);
      uint32_t groupLastDoc = prevLastDoc + groupLastDocDelta;
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - block);
      int32_t groupDocCount = std::min(L1_DOCS, docfreq - group * L1_DOCS);
      int64_t groupTfSum = groupDocCount;
      if (hasPositions) {
        groupTfSum += InputStream::readVint(p, groupHeaderEnd);
      }
      if (hasFreqs && hasNorms) {
        uint32_t frontierCount = InputStream::readVint(p, groupHeaderEnd);
        for (uint32_t i = 0; i < frontierCount; i++) {
          assert(p < groupHeaderEnd);
          p++;  // norm: one raw byte
          auto tfDelta = InputStream::readVint(p, groupHeaderEnd);
          unused(tfDelta);
        }
      } else if (hasFreqs) {
        auto spanImpact = InputStream::readVint(p, groupHeaderEnd);
        unused(spanImpact);
      } else if (hasNorms) {
        auto spanMinNorm = InputStream::readVint(p, groupHeaderEnd);
        unused(spanMinNorm);
      }
      assert(p == groupHeaderEnd);

      const char* groupBody = groupHeaderEnd;
      if (target <= (int32_t) groupLastDoc) {
        p = groupBody;
        if (walkL0To(block + groupBlockCount)) {
          return;
        }
        assert(false);
        return;
      }

      p = groupBody + (int64_t) groupByteLen;
      assert(p <= end);
      prevLastDoc = groupLastDoc;
      cumTf += groupTfSum;
      block += groupBlockCount;
      nextL1Group = block / L1_PERIOD;
      nextL1Base = prevLastDoc;
      nextL1CumTf = cumTf;
    }

    docIS.seek(p - streamStart);
    docOrd = docfreq;
    tfreqOrd = docfreq;
    cumulativeTermFreq = cumTf;
    docBufIdx = docBufEnd = 0;
    tfreqBufIdx = tfreqBufEnd = 0;
    nextL0Block = numDocBlocks;
    nextL0Base = prevLastDoc;
    nextL0CumTf = cumTf;
    nextL1Group = numDocGroups;
    nextL1Base = prevLastDoc;
    nextL1CumTf = cumTf;
    bodyReady = false;
  }

  // Strict (like the Scorer / Lucene PostingsEnum contract): target must be beyond
  // the current doc.  Callers: ConjunctionScorer / MandOpt / MandNot, PhraseQuery,
  // ConstantScoreQuery, the column-join iterators.
  int32_t advance(int32_t target) {
    assert(docid < target);
    skipCount(SkipStats::advanceCalls);
    if (nextL0Block < numDocBlocks
        && (docBufEnd == 0 || target > docBuf[docBufEnd - 1])) {
      skipToBlock(target);
    }
    if (docBufIdx >= docBufEnd) {
      // fresh block (or END) after the block-level skip
      if (nextDoc() >= target) {
        return docid;
      }
    }
    if (docBuf[docBufEnd - 1] < target) {
      // target is past the final decoded block (last block of the list, or a
      // pulsed posting): walk out to END the simple way.
      while (docid < target) {
        nextDoc();
      }
      return docid;
    }

    // The target lies in the decoded remainder: jump to it directly instead of
    // a per-doc nextDoc() walk. Plain term scorers do not consume positions, so
    // they skip cumulative-tf repair entirely.
    blockMode = false;
    const int32_t start = docBufIdx;
    const int32_t j = (int32_t) (std::lower_bound(docBuf + start, docBuf + docBufEnd, target)
                                 - docBuf);
    assert(j < docBufEnd);
    const int32_t consumed = j + 1 - start;
    docOrd += consumed;
    if (hasFreqs) {
      tfreq = tfreqBuf[j];
      tfreqOrd += consumed;
      tfreqBufIdx = j + 1;
      if (trackPositions) {
        int64_t sum = 0;
        for (int32_t i = start; i <= j; i++) {
          sum += (uint32_t) tfreqBuf[i];
        }
        cumulativeTermFreq += sum;
        posOrdStart = cumulativeTermFreq - tfreq;
      }
    } else {
      tfreq = 1;
      if (trackPositions) {
        cumulativeTermFreq += consumed;
        posOrdStart = cumulativeTermFreq - tfreq;
      }
    }
    docBufIdx = j + 1;
    docid = docBuf[j];
    return docid;
  }

  // Read stored impact data without decoding postings bodies.  DOCS-only fields
  // do not store impact fields, so their per-block maxTf and group span impacts are
  // synthesized as 1; fields without norms synthesize minNorm/spanMinNorm as 0.
  void readBlockMaxTf(std::vector<int32_t>& blockMaxTf,
                      std::vector<int32_t>* groupSpanImpacts = nullptr,
                      std::vector<int32_t>* blockLastDocs = nullptr,
                      std::vector<int32_t>* blockMinNorms = nullptr,
                      std::vector<int32_t>* groupSpanMinNorms = nullptr,
                      ImpactFrontiers* impactFrontiers = nullptr) const {
    blockMaxTf.resize(0);
    if (groupSpanImpacts != nullptr) {
      groupSpanImpacts->resize(0);
    }
    if (blockLastDocs != nullptr) {
      blockLastDocs->resize(0);
    }
    if (blockMinNorms != nullptr) {
      blockMinNorms->resize(0);
    }
    if (groupSpanMinNorms != nullptr) {
      groupSpanMinNorms->resize(0);
    }
    if (impactFrontiers != nullptr) {
      impactFrontiers->clear();
    }
    if (docsSize == 0) {
      return;
    }

    blockMaxTf.reserve(numDocBlocks);
    if (groupSpanImpacts != nullptr) {
      groupSpanImpacts->reserve(numDocGroups);
    }
    if (blockLastDocs != nullptr) {
      blockLastDocs->reserve(numDocBlocks);
    }
    if (blockMinNorms != nullptr) {
      blockMinNorms->reserve(numDocBlocks);
    }
    if (groupSpanMinNorms != nullptr) {
      groupSpanMinNorms->reserve(numDocGroups);
    }
    if (impactFrontiers != nullptr) {
      impactFrontiers->offsets.reserve((size_t) numDocBlocks + 1);
    }

    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr(startOfDocs);
    uint32_t prevGroupLastDoc = 0;
    uint32_t prevBlockLastDoc = 0;

    for (int32_t group = 0; group < numDocGroups; group++) {
      int32_t groupStartBlock = group * L1_PERIOD;
      int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - groupStartBlock);

      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDoc = prevGroupLastDoc + readVint15(p, groupHeaderEnd);
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      if (hasPositions) {
        auto groupCumTfDelta = InputStream::readVint(p, groupHeaderEnd);
        unused(groupCumTfDelta);
      }
      // The group corner (max tf, min norm) is the staircase's last tf and
      // first norm for freqs+norms fields; stored directly otherwise.
      int32_t spanImpact = 1;
      int32_t spanMinNorm = 0;
      if (hasFreqs && hasNorms) {
        uint32_t frontierCount = InputStream::readVint(p, groupHeaderEnd);
        assert(frontierCount > 0);
        int32_t tf = 0;
        for (uint32_t i = 0; i < frontierCount; i++) {
          assert(p < groupHeaderEnd);
          int32_t norm = (int32_t) (uint8_t) *p;  // raw byte (absolute)
          p++;
          tf += (int32_t) InputStream::readVint(p, groupHeaderEnd);  // tf delta
          if (i == 0) {
            spanMinNorm = norm;
          }
        }
        spanImpact = tf;
      } else if (hasFreqs) {
        spanImpact = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      } else if (hasNorms) {
        spanMinNorm = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      }
      assert(p == groupHeaderEnd);
      if (groupSpanImpacts != nullptr) {
        groupSpanImpacts->push_back(spanImpact);
      }
      if (groupSpanMinNorms != nullptr) {
        groupSpanMinNorms->push_back(spanMinNorm);
      }

      const char* const groupBody = groupHeaderEnd;
      const char* const groupEnd = groupBody + (int64_t) groupByteLen;
      assert(groupEnd <= end);
      p = groupBody;

      for (int32_t i = 0; i < groupBlockCount; i++) {
        uint32_t headerLen = InputStream::readVint(p, groupEnd);
        const char* headerEnd = p + headerLen;
        assert(headerEnd <= groupEnd);
        uint32_t blockLastDocValue = prevBlockLastDoc + readVint15(p, headerEnd);
        uint64_t blockByteLen = readVlong15(p, headerEnd);
        if (hasPositions) {
          auto blockCumTfDelta = InputStream::readVint(p, headerEnd);
          unused(blockCumTfDelta);
          auto blockPosByteOff = InputStream::readVlong(p, headerEnd);
          unused(blockPosByteOff);
        }
        int32_t maxTf = 1;
        int32_t minNorm = 0;
        if (impactFrontiers != nullptr) {
          impactFrontiers->offsets.push_back((int32_t) impactFrontiers->tfs.size());
        }
        if (hasFreqs && hasNorms) {
          uint32_t frontierCount = InputStream::readVint(p, headerEnd);
          assert(frontierCount > 0);
          int32_t tf = 0;
          for (uint32_t j = 0; j < frontierCount; j++) {
            assert(p < headerEnd);
            int32_t norm = (int32_t) (uint8_t) *p;  // raw byte (absolute)
            p++;
            tf += (int32_t) InputStream::readVint(p, headerEnd);  // tf delta
            assert(norm >= 0 && norm <= 255);
            assert(tf > 0);
            if (j == 0) {
              minNorm = norm;
            }
            maxTf = tf;
            if (impactFrontiers != nullptr) {
              impactFrontiers->norms.push_back(norm);
              impactFrontiers->tfs.push_back(tf);
            }
          }
        } else if (hasFreqs) {
          maxTf = (int32_t) InputStream::readVint(p, headerEnd);
        } else if (hasNorms) {
          minNorm = (int32_t) InputStream::readVint(p, headerEnd);
        }
        assert(p == headerEnd);
        blockMaxTf.push_back(maxTf);
        if (blockLastDocs != nullptr) {
          blockLastDocs->push_back((int32_t) blockLastDocValue);
        }
        if (blockMinNorms != nullptr) {
          blockMinNorms->push_back(minNorm);
        }
        p = headerEnd + (int64_t) blockByteLen;
        assert(p <= groupEnd);
        prevBlockLastDoc = blockLastDocValue;
      }

      assert(p == groupEnd);
      prevGroupLastDoc = groupLastDoc;
      assert(prevBlockLastDoc == prevGroupLastDoc);
    }

    assert((int32_t) blockMaxTf.size() == numDocBlocks);
    assert(blockMinNorms == nullptr || (int32_t) blockMinNorms->size() == numDocBlocks);
    if (impactFrontiers != nullptr) {
      impactFrontiers->offsets.push_back((int32_t) impactFrontiers->tfs.size());
      assert((int32_t) impactFrontiers->offsets.size() == numDocBlocks + 1);
      assert(impactFrontiers->tfs.size() == impactFrontiers->norms.size());
    }
    assert(p == end);
  }

  /// total number of doc blocks (L0) for this term - sizes impact indexes
  int32_t numImpactBlocks() const {
    return numDocBlocks;
  }

  // Group-level impact scan: walks ONLY the L1 group headers, hopping each
  // group's body via its byte length - ~df/4096 steps instead of the whole-
  // list walk readBlockMaxTf does.  Emits per group: last doc, the group's
  // stored impact frontier (with its (maxTf, minNorm) corner derived for
  // consumers that want a single span), and what a later lazy parse of that
  // group's L0 headers needs (body offset relative to startOfDocs, delta
  // bases).  Fields without freqs+norms have empty frontier ranges.
  struct GroupImpacts {
    std::vector<int32_t> lastDocs;
    std::vector<int32_t> spanMaxTfs;
    std::vector<int32_t> spanMinNorms;
    std::vector<int64_t> bodyOffsets;   // relative to startOfDocs
    std::vector<int32_t> baseLastDocs;  // last doc before the group (L0 delta base)
    ImpactFrontiers frontiers;          // per-group frontier staircases
  };

  void readGroupImpacts(GroupImpacts& out) const {
    out.lastDocs.resize(0);
    out.spanMaxTfs.resize(0);
    out.spanMinNorms.resize(0);
    out.bodyOffsets.resize(0);
    out.baseLastDocs.resize(0);
    out.frontiers.clear();
    if (docsSize == 0) {
      return;
    }
    out.lastDocs.reserve(numDocGroups);
    out.spanMaxTfs.reserve(numDocGroups);
    out.spanMinNorms.reserve(numDocGroups);
    out.bodyOffsets.reserve(numDocGroups);
    out.baseLastDocs.reserve(numDocGroups);
    out.frontiers.offsets.reserve((size_t) numDocGroups + 1);

    const char* const streamStart = docIS.ptr(0);
    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr(startOfDocs);
    uint32_t prevGroupLastDoc = 0;
    for (int32_t group = 0; group < numDocGroups; group++) {
      uint32_t groupHeaderLen = InputStream::readVint(p, end);
      const char* groupHeaderEnd = p + groupHeaderLen;
      assert(groupHeaderEnd <= end);
      uint32_t groupLastDoc = prevGroupLastDoc + readVint15(p, groupHeaderEnd);
      uint64_t groupByteLen = readVlong15(p, groupHeaderEnd);
      if (hasPositions) {
        auto groupCumTfDelta = InputStream::readVint(p, groupHeaderEnd);
        unused(groupCumTfDelta);
      }
      int32_t spanImpact = 1;
      int32_t spanMinNorm = 0;
      out.frontiers.offsets.push_back((int32_t) out.frontiers.tfs.size());
      if (hasFreqs && hasNorms) {
        uint32_t frontierCount = InputStream::readVint(p, groupHeaderEnd);
        assert(frontierCount > 0);
        int32_t tf = 0;
        for (uint32_t i = 0; i < frontierCount; i++) {
          assert(p < groupHeaderEnd);
          int32_t norm = (int32_t) (uint8_t) *p;  // raw byte (absolute)
          p++;
          tf += (int32_t) InputStream::readVint(p, groupHeaderEnd);  // tf delta
          if (i == 0) {
            spanMinNorm = norm;
          }
          out.frontiers.norms.push_back(norm);
          out.frontiers.tfs.push_back(tf);
        }
        spanImpact = tf;
      } else if (hasFreqs) {
        spanImpact = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      } else if (hasNorms) {
        spanMinNorm = (int32_t) InputStream::readVint(p, groupHeaderEnd);
      }
      assert(p == groupHeaderEnd);
      out.baseLastDocs.push_back((int32_t) prevGroupLastDoc);
      out.lastDocs.push_back((int32_t) groupLastDoc);
      out.spanMaxTfs.push_back(spanImpact);
      out.spanMinNorms.push_back(spanMinNorm);
      out.bodyOffsets.push_back((int64_t) (groupHeaderEnd - streamStart) - startOfDocs);
      p = groupHeaderEnd + (int64_t) groupByteLen;
      assert(p <= end);
      prevGroupLastDoc = groupLastDoc;
    }
    out.frontiers.offsets.push_back((int32_t) out.frontiers.tfs.size());
    assert(p == end);
  }

  // Parse ONE group's L0 block headers, located by readGroupImpacts output.
  template <typename Visitor>
  void visitGroupBlockImpacts(int32_t groupIndex, int64_t bodyOffset, int32_t baseLastDoc,
                              GroupBlockImpactScratch& scratch, Visitor&& visitor) const {
    int32_t groupStartBlock = groupIndex * L1_PERIOD;
    int32_t groupBlockCount = std::min(L1_PERIOD, numDocBlocks - groupStartBlock);
    assert(groupBlockCount > 0);

    const char* const end = docIS.ptr(endOfDocs);
    const char* p = docIS.ptr(startOfDocs + bodyOffset);
    uint32_t prevBlockLastDoc = (uint32_t) baseLastDoc;
    for (int32_t i = 0; i < groupBlockCount; i++) {
      uint32_t headerLen = InputStream::readVint(p, end);
      const char* headerEnd = p + headerLen;
      assert(headerEnd <= end);
      uint32_t blockLastDocValue = prevBlockLastDoc + readVint15(p, headerEnd);
      uint64_t blockByteLen = readVlong15(p, headerEnd);
      if (hasPositions) {
        auto blockCumTfDelta = InputStream::readVint(p, headerEnd);
        unused(blockCumTfDelta);
        auto blockPosByteOff = InputStream::readVlong(p, headerEnd);
        unused(blockPosByteOff);
      }
      int32_t maxTf = 1;
      int32_t minNorm = 0;
      int32_t frontierStored = 0;
      bool frontierSpilled = false;
      if (hasFreqs && hasNorms) {
        uint32_t frontierCount = InputStream::readVint(p, headerEnd);
        frontierSpilled = frontierCount > (uint32_t) GroupBlockImpactScratch::FRONTIER_CAP;
        if (frontierSpilled) skipCount(SkipStats::impactL0GroupParseScratchSpills);
        assert(frontierCount > 0);
        int32_t tf = 0;
        for (uint32_t j = 0; j < frontierCount; j++) {
          assert(p < headerEnd);
          int32_t norm = (int32_t) (uint8_t) *p;
          p++;
          tf += (int32_t) InputStream::readVint(p, headerEnd);
          if (j == 0) {
            minNorm = norm;
          }
          maxTf = tf;
          if (!frontierSpilled) {
            scratch.frontierNorms[frontierStored] = norm;
            scratch.frontierTfs[frontierStored] = tf;
            frontierStored++;
          }
        }
      } else if (hasFreqs) {
        maxTf = (int32_t) InputStream::readVint(p, headerEnd);
      } else if (hasNorms) {
        minNorm = (int32_t) InputStream::readVint(p, headerEnd);
      }
      assert(p == headerEnd);
      visitor(i, (int32_t) blockLastDocValue, maxTf, minNorm,
              std::span<const int32_t>(scratch.frontierTfs, (size_t) frontierStored),
              std::span<const int32_t>(scratch.frontierNorms, (size_t) frontierStored),
              frontierSpilled);
      p = headerEnd + (int64_t) blockByteLen;
      assert(p <= end);
      prevBlockLastDoc = blockLastDocValue;
    }
  }

  void startPositions() {
    assert(hasPositions);  // positions are only queried on fields that index them
    assert(trackPositions);
    pos = -1;
    while (posOrd < posOrdStart) {
      // need to skip some positions.
      auto numToSkip = posOrdStart - posOrd;
      auto leftInBlock = posBufEnd - posBufIdx;

      if (numToSkip <= leftInBlock) {
        // std::cout << "skipping within position block: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        // our start is within current decoded block
        posBufIdx += numToSkip;
        posOrd += numToSkip;
        posBufEndDoc = std::min(posBufIdx + tfreq, posBufEnd);
        return;
      }

      // skip to end of block first.
      // some of these calculations may be redundant, but it's just to make it easier to think about for now.
      // Hopefully optimizer can take care of inefficiencies.
      posOrd += leftInBlock;
      numToSkip -= leftInBlock;
      posBufIdx = posBufEnd;

      if (numToSkip >= Postings::POSITIONS_BLOCK_SIZE) {
        // Skip the entire encoded block from its header - no unpack. posOrd is
        // block-aligned here (initial state or driven to a boundary above), and
        // numToSkip >= a full block guarantees the next block is block-encoded,
        // exactly as the old decode-and-discard relied on.
        auto bytesSkipped = IndexCodec::posCodec.skipBlock(posIS.ptr(), posIS.left());
        posIS.skip(bytesSkipped);
        posOrd += Postings::POSITIONS_BLOCK_SIZE;
        skipCount(SkipStats::posBlocksSkipped);
        // Fully reset buffer state: a skip that lands exactly on posOrdStart
        // exits the loop without another decode, and nextPosition() must then
        // take its fresh-decode path (a stale posBufEndDoc < posBufEnd would
        // send it into the still-buffered branch and read past posBuf).
        posBufIdx = posBufEnd = posBufEndDoc = 0;
        continue;
      }


      // At this point, we need to skip less than a block of positions, but we need to know
      // if it is block encoded or vInt encoded.  Compare to last block encoded ord to tell.
      // Boundary conditions: if posOrdStart=127 then it is the last in the block and we
      // do want to decode the block (hence do tail logic otherwise)
      if (posOrdStart > lastBlockEncodedPosOrd(ttf)) {
        // std::cout << "skipping in position tail: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        for (int i=0; i<numToSkip; i++) {
          auto delta = posIS.readVint();
          unused(delta);
          // TODO: a faster skipVint (potentially)? inlining should already eliminate the dead code though.
        }
        posOrd += numToSkip;
        assert (posOrd == posOrdStart); // nocommit, trivial
        posBufIdx = posBufEnd = posBufEndDoc = 0;
        return;
      }

      // OK load block of positions.  This could be optimized by only loading the relevant part.
      // If this enum wants all positions, we should just decode everything.
      uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
      auto bytesRead = IndexCodec::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
      posIS.skip(bytesRead);
      assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
      skipCount(SkipStats::posBlocksDecoded);
      posBufIdx = 0;
      posBufEnd = outSz;
      posBufEndDoc = std::min(posBufEnd, tfreq);
    }
  }

  int32_t nextPosition() {
    if (posBufIdx >= posBufEndDoc) {
      // Reached the end of buffered pos deltas, either because
      // there are no more positions for this doc, or because we need
      // to reload more.

      int leftToRead = (int)(cumulativeTermFreq - posOrd); // should never be larger than 32 bit int

      if (leftToRead <= 0) {
        assert(posOrd == cumulativeTermFreq); // should not have gone past
        // TODO: set pos to something, or keep last valid position?
        pos = PostingsReader::END;
        return pos;
      }

      // we moved to a new doc, but still have positions decoded in the buffer.
      // TODO: it feels like we should move this case to startPositions()
      if (posBufEnd > posBufEndDoc) {
        // just update our new end pointer
        posBufEndDoc = std::min(posBufEnd, posBufIdx+leftToRead);
      } else {
        // if we have more positions to read, then there should be no more buffered
        assert(posBufEndDoc == posBufEnd);

        if (posOrd <= lastBlockEncodedPosOrd(ttf)) {
          // read a new block of positions
          // TODO: refactor reading a new block (not skipping) to one place?
          uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
          auto bytesRead = IndexCodec::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
          posIS.skip(bytesRead);
          assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
          skipCount(SkipStats::posBlocksDecoded);
          posBufIdx = 0;
          posBufEnd = outSz;
          // This doc may have started in the previous positions block.
          posBufEndDoc = std::min(posBufEnd, leftToRead);
        } else {
          // we are in the tail
          // we could read position-by-position at the tail if we wanted...
          // we could also handle pulsed positions here instead if sticking them in the buffer.
          posBufIdx = 0;
          posBufEnd = leftToRead; // only read enough for this doc
          posBufEndDoc = leftToRead;
          // std::cout << "FILL pos buffer docid=" << docid << " ords=" << posOrd << " through " << posOrd+tfreq-1 << std::endl;
          for (int i = posBufIdx; i < posBufEnd; i++) {
            posBuf[i] = posIS.readVint();
          }
        }
      }
    }
    // std::cout << "RETN pos buffer docid=" << docid << " posOrd=" << posOrd << " posBufIdx=" << posBufIdx << std::endl;
    auto delta = posBuf[posBufIdx++];
    // We could possibly move posOrd update and only update at block end.
    // Would need to adjust/account for where we started in a block though.
    // And given that there is no data dependency in this hot loop, it's unclear if it would help at all.
    posOrd++;
    pos += delta;
    return pos;
  }

  int32_t advancePosition(int32_t target) {
    while (pos < target) {
      nextPosition();
    }
    return pos;
  }

  // We could also think about exposing the position deltas?
  // Or, we could sum the positions in a block (for a doc) and then it would make bulk access
  // easier / more efficient.
  // Also think about bulk copying of positions when merging?  Would only work for first segment unless
  // we supported multiple position lists.  Hence prob only worth it when merging small segment into large.
  // How to do position skipping?  Could always pre-pend last position in a block?
};

}
