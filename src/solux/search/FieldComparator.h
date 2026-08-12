#pragma once

#include "solux/search/Collector.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/search/OrdMap.h"
#include "solux/util/MemPool.h"
#include "solux/util/solux_util.h"
#include <bit>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <limits>
#include <cassert>
#include <optional>
#include <span>

namespace solux {

class FieldComparator {
public:
  // Optional batch sort-key capability for comparators whose complete sort
  // order is an ascending transformed int64 key. Valid for the current segment
  // only; setSegment resets its cursors. Implementations must not depend on
  // setBottom notifications while this capability is in use.
  // NOTE: no virtual destructor; never owned or deleted through this type.
  class KeyBatch {
  public:
    // Live comparator slot storage, read and written directly by the collector.
    std::span<int64_t> slotKeys;

    // Gather transformed keys for ascending in-segment docs.
    virtual void gatherKeys(std::span<const int32_t> docs,
                            std::span<int64_t> keys) = 0;

    // Optional block-granular pruning bounds over the current segment. A
    // nonzero size means doc d reads its key from block d / keyBlockSize()
    // and blockBestKey(b) lower-bounds the transformed key of every doc in
    // block b. Zero means no per-block bounds (collection stays exhaustive).
    virtual int32_t keyBlockSize() const { return 0; }
    virtual int64_t keyBlockCount() const { return 0; }
    virtual int64_t blockBestKey(int64_t block) const {
      unused(block);
      assert(false);
      return std::numeric_limits<int64_t>::min();
    }

    // Order-independent fused block gather: extract one key block's in-domain
    // docs and transformed keys straight from the domain bitset, without a
    // doc-array round trip. `words` is the whole-segment bitset (word w
    // covers docs [64w, 64w+64)); an EMPTY span means no mask - every doc in
    // the block (match-all with no deletes). Blocks may be requested in ANY
    // order - only comparators with no forward-only cursor (dense
    // single-valued) offer this. outDocs/outKeys must hold keyBlockSize()
    // entries. Returns the number gathered.
    virtual bool supportsMaskedBlockGather() const { return false; }
    virtual int32_t gatherBlockMasked(int64_t block,
                                      std::span<const uint64_t> words,
                                      std::span<int32_t> outDocs,
                                      std::span<int64_t> outKeys) {
      unused(block, words, outDocs, outKeys);
      assert(false);
      return 0;
    }
  };

  virtual ~FieldComparator() = default;

  // Set the current segment for this comparator
  virtual void setSegment(int32_t segment, PostingsReader* reader) = 0;

  virtual void setBottom(int32_t slot) { unused(slot); }

  // compare new document to bottom slot
  virtual int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) = 0;

  // compare two slots
  virtual int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) = 0;

  // compare a slot in this comparator with a slot in another comparator
  virtual int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) = 0;

  // Copy the value from a document to a slot.
  virtual void copy(int32_t slot, segdoc doc) = 0;

  // Copy the value from a different comparator to this comparator
  virtual void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) = 0;

  // Grow per-slot storage to hold at least `capacity` slots.  The collector
  // mints slots densely and calls this before the first write to a new slot,
  // so storage need not be allocated for the worst-case bound up front.
  // Implementations that publish KeyBatch::slotKeys must re-point it here.
  virtual void growSlots(int32_t capacity) = 0;

  virtual KeyBatch* keyBatch() { return nullptr; }

  // Optional competitive segment-ord interval for string-sort pruning,
  // derived from the heap bottom (so unlike KeyBatch it REQUIRES the
  // setBottom lifecycle). READY yields the inclusive 1-based segment-ord
  // interval [lo, hi] of terms whose docs could still enter the heap
  // (lo > hi: none can - the segment is done). PENDING means no usable
  // bound yet (e.g. the bottom is a missing value); UNSUPPORTED means this
  // comparator or segment can never provide intervals.
  //
  // `from` is the collection cursor: every doc still to be collected has
  // segdoc(segment, doc) >= segdoc(segment, from). The boundary (equal to
  // bottom) term is excluded only for a sole-clause sort with an exact
  // bottom whose segdoc tie-break every remaining doc loses.
  enum class OrdIntervalStatus { UNSUPPORTED, PENDING, READY };
  struct CompetitiveOrdState {
    int64_t lo = 0;
    int64_t hi = -1;
    int64_t nTerms = 0;
    PostingsReader* postingsReader = nullptr;
    const SegFieldInfo* fieldInfo = nullptr;
  };
  virtual OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) {
    unused(out, bottomSlot, bottomDoc, segment, from, soleSort);
    return OrdIntervalStatus::UNSUPPORTED;
  }

  enum MissingValue {
    MISSING_FIRST,
    MISSING_LAST
  };
};



// A comparator that can be used in a simplified collector that has values
// that can be compared across different segments.
class SimpleNumericFieldComparator : public FieldComparator,
                                     public FieldComparator::KeyBatch {
  // IntColBM break-even: decodeFrame wins at eight gathered values per frame.
  static constexpr size_t FRAME_DECODE_MIN = 8;

  std::string fieldName;
  std::optional<IntColReader> reader;
  std::optional<IntColReader::Iterator> iter;
  std::optional<IntColReader::SparseValues> pointValues;
  std::optional<screaming::BitSet::Iterator> docsIter;
  std::vector<int64_t> values; // Storage for bottom values
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingValueSubstitute;
  int64_t lastValue = 0;  // TODO: cache the last value lookup in compareBottom so we don't have to re-fetch if competitive
  segdoc lastDoc = {-1, -1};
  int64_t decoded[NumColumn::BULK_SIZE];
  int64_t decodedStart = -1;
  int64_t decodedEnd = -1;
  int32_t landedDoc = -1;
  int32_t landedRank = -1;
  bool multiValued = false;

  int64_t transformPresent(int64_t value) const {
    return sortMultiplier < 0 ? ~value : value;
  }

  void gatherDenseSingle(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    size_t i = 0;
    while (i < docs.size()) {
      int64_t frameStart =
          (int64_t)docs[i] / NumColumn::BULK_SIZE * NumColumn::BULK_SIZE;
      int64_t frameEnd = frameStart + NumColumn::BULK_SIZE;
      size_t groupEnd = i + 1;
      while (groupEnd < docs.size() && docs[groupEnd] < frameEnd) {
        groupEnd++;
      }

      if (decodedStart == frameStart) {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else if (groupEnd - i >= FRAME_DECODE_MIN) {
        uint32_t count;
        decodedStart = reader->decodeValueSubBlock(docs[i], decoded, count);
        decodedEnd = decodedStart + count;
        for (; i < groupEnd; i++) {
          assert(docs[i] >= decodedStart && docs[i] < decodedEnd);
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(pointValues->valueAt(docs[i]));
        }
      }
    }
  }

  bool land(int32_t doc) {
    if (doc > landedDoc) {
      landedDoc = docsIter->advance(doc);
      landedRank = docsIter->rank();
    }
    return doc == landedDoc;
  }

  void gatherSparseSingle(std::span<const int32_t> docs,
                          std::span<int64_t> keys) {
    for (size_t i = 0; i < docs.size(); i++) {
      if (!land(docs[i])) {
        keys[i] = missingValueSubstitute;
      } else {
        keys[i] = transformPresent(pointValues->valueAt(landedRank));
      }
    }
  }

  void gatherMultiValued(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    bool dense = !docsIter.has_value();
    for (size_t i = 0; i < docs.size(); i++) {
      int32_t rank;
      if (dense) {
        rank = docs[i];
      } else if (land(docs[i])) {
        rank = landedRank;
      } else {
        keys[i] = missingValueSubstitute;
        continue;
      }
      int64_t valueRank = reader->getStartValueRank(rank);
      keys[i] = transformPresent(pointValues->valueAt(valueRank));
    }
  }

public:
  SimpleNumericFieldComparator(const std::string& fieldName, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      sortMultiplier(reversed ? -1 : 1) {

    // Missing placement is a fixed edge regardless of direction (the string
    // comparators and expression sort keys follow the same convention), so
    // the substitute is chosen in the TRANSFORMED key space: getDocValue
    // ~-maps present values for descending sorts, and an untransformed
    // extreme lands at the same output edge either way.
    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    iter.reset();
    pointValues.reset();
    docsIter.reset();
    reader.reset();
    multiValued = false;
    decodedStart = -1;
    decodedEnd = -1;
    landedDoc = -1;
    landedRank = -1;

    if (!postingsReader) return;

    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
        pointValues.emplace(*reader);
        multiValued = reader->multiValued();
        if (!reader->denseDocsWithValue()) {
          docsIter.emplace(reader->docsWithValueBitSet());
        }
      }
    }
  }

  FieldComparator::KeyBatch* keyBatch() override {
    return this;
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity <= values.size()) return;
    resizeExact(values, (size_t)capacity);
    slotKeys = values;
  }

  // Dense single-valued columns map doc == value rank, so the per-4096-value
  // zone maps are exact per-doc-block key bounds. Sparse and multi-valued
  // columns mix ranks and docs, so no bounds are offered there.
  int32_t keyBlockSize() const override {
    if (!reader.has_value() || multiValued || docsIter.has_value()) return 0;
    return (int32_t)NumColumnFormat::BLOCK_SIZE;
  }

  int64_t keyBlockCount() const override {
    return reader->numBlocks();
  }

  int64_t blockBestKey(int64_t block) const override {
    NumBlockZone zone = reader->blockZone(block);
    return sortMultiplier < 0 ? ~zone.max : zone.min;
  }

  void gatherKeys(std::span<const int32_t> docs,
                  std::span<int64_t> keys) override {
    assert(keys.size() == docs.size());
    if (!reader.has_value()) {
      std::fill(keys.begin(), keys.end(), missingValueSubstitute);
    } else if (multiValued) {
      gatherMultiValued(docs, keys);
    } else if (docsIter.has_value()) {
      gatherSparseSingle(docs, keys);
    } else {
      gatherDenseSingle(docs, keys);
    }
  }

  // Dense single-valued only: doc == value rank and no forward-only landing
  // cursor, so blocks can be gathered in any order.
  bool supportsMaskedBlockGather() const override {
    return keyBlockSize() != 0;
  }

  int32_t gatherBlockMasked(int64_t block, std::span<const uint64_t> words,
                            std::span<int32_t> outDocs,
                            std::span<int64_t> outKeys) override {
    assert(keyBlockSize() != 0);
    int64_t blockStart = block * (int64_t)NumColumnFormat::BLOCK_SIZE;
    if (words.empty()) {
      // No mask: every doc in the block (dense single-valued, doc == rank).
      int64_t blockLimit = std::min(blockStart + NumColumnFormat::BLOCK_SIZE,
                                    reader->numValues());
      int32_t out = 0;
      for (int64_t frameStart = blockStart; frameStart < blockLimit;
           frameStart += NumColumn::BULK_SIZE) {
        if (decodedStart != frameStart) {
          uint32_t count;
          decodedStart =
              reader->decodeValueSubBlock((int32_t)frameStart, decoded, count);
          decodedEnd = decodedStart + count;
        }
        int64_t frameEnd = std::min(frameStart + NumColumn::BULK_SIZE,
                                    blockLimit);
        for (int64_t doc = frameStart; doc < frameEnd; doc++) {
          outDocs[(size_t)out] = (int32_t)doc;
          outKeys[(size_t)out] = transformPresent(decoded[doc - decodedStart]);
          out++;
        }
      }
      return out;
    }
    int64_t blockLimit = std::min(blockStart + NumColumnFormat::BLOCK_SIZE,
                                  (int64_t)words.size() * 64);
    int32_t out = 0;
    for (int64_t frameStart = blockStart; frameStart < blockLimit;
         frameStart += NumColumn::BULK_SIZE) {
      size_t w0 = (size_t)(frameStart >> 6);
      size_t wEnd = std::min(w0 + NumColumn::BULK_SIZE / 64, words.size());
      int32_t frameCard = 0;
      for (size_t w = w0; w < wEnd; w++) {
        frameCard += (int32_t)std::popcount(words[w]);
      }
      if (frameCard == 0) continue;
      bool useDecoded = frameCard >= (int32_t)FRAME_DECODE_MIN;
      if (useDecoded && decodedStart != frameStart) {
        uint32_t count;
        decodedStart =
            reader->decodeValueSubBlock((int32_t)frameStart, decoded, count);
        decodedEnd = decodedStart + count;
      }
      for (size_t w = w0; w < wEnd; w++) {
        uint64_t bits = words[w];
        int32_t base = (int32_t)(w << 6);
        while (bits != 0) {
          int32_t doc = base + std::countr_zero(bits);
          bits &= bits - 1;
          outDocs[(size_t)out] = doc;
          outKeys[(size_t)out] = transformPresent(
              useDecoded ? decoded[doc - decodedStart]
                         : pointValues->valueAt(doc));
          out++;
        }
      }
    }
    return out;
  }

  virtual int64_t getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return missingValueSubstitute;
    }

    if (iter->docId() < docid) {
      iter->advance(docid);
    }
    if (iter->docId() > docid) {
      return missingValueSubstitute;
    }
    // Multi-valued numeric columns store per-doc values unsorted, so the
    // cheap deterministic sort key is the FIRST stored value (storage order).
    // min/max over unsorted values would scan every value per doc; callers
    // that want that spell it explicitly (min(f)/max(f) sort expressions).
    int64_t value = multiValued
        ? iter->values().valueAt(reader->getStartValueRank(iter->rank()))
        : iter->value();
    // For descending sort, negate the value.
    // if we wanted to make this branchless, we could have a an adder and multiplier
    // but this will be a predictable branch anyway.
    if (sortMultiplier < 0) {
      // Descending: ~value == -(value+1) but with no signed-overflow UB.
      // -(value+1) overflows when value==INT64_MAX (and negating INT64_MIN is
      // UB too); ~value is the same order-reversing map over the full range.
      value = ~value;
    }
    return value;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    // SimpleNumericFieldComparator can ignore segments when comparing values
    // since numeric values are comparable across segments
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    int64_t valA = values[slotA];
    int64_t valB = values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  // NOINLINE: keeps the per-doc rejection path a single out-of-line unit with the
  // column iterator inlined into it, rather than speculative-devirt inlining a
  // half of it into the collect loop and spilling value()/advance() to calls.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    // Get the bottom value from the slot
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    int64_t bottomVal = values[bottomSlot];

    // Get the new document's value
    int64_t newVal = getDocValue(newDoc.docId());

    return (bottomVal > newVal) - (bottomVal < newVal);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    int64_t value = getDocValue(doc.docId());
    values[slot] = value;
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    // For numeric comparators, we can compare the stored values directly
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherNumeric->values.size());
    int64_t valA = values[slotA];
    int64_t valB = otherNumeric->values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    // Copy value from another comparator
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherNumeric->values.size());
    values[slot] = otherNumeric->values[otherSlot];
  }
  
  ~SimpleNumericFieldComparator() override = default;
};

// A comparator for string fields that uses global ordinals for efficient cross-segment comparison
// Selected segment ord for sorting: single-valued docs read their one ord;
// multi-valued docs sort by their smallest ord ascending and largest ord
// descending (min/max term semantics). Returns 0 for missing.
inline int32_t selectedSortOrd(const OrdColReader& reader, int32_t docid,
                               bool descending) {
  if (!reader.multiValued()) return reader.ordAt(docid);
  if (!descending) return reader.firstOrdAt(docid);
  const DocsReader& docs = reader.docsReader();
  int32_t docRank;
  if (!docs.hasBitset()) {
    docRank = docid;
  } else {
    screaming::BitSet::Iterator iter(docs.bitset());
    if (iter.advance(docid) != docid) return 0;
    docRank = iter.rank();
  }
  auto [start, end] = reader.getStartEndValueRank(docRank);
  assert(start < end);
  OrdColReader::PointOrds ords(reader);
  return ords.valueAt(end - 1);
}

class GlobalOrdComparator : public FieldComparator {
  std::string fieldName;
  std::shared_ptr<OrdMap> ordMap;
  std::optional<OrdColReader> reader;
  std::vector<int64_t> globalOrds; // Storage for global ordinals
  OrdMap::SegToGlobal segmentMapping;
  PostingsReader* postingsReader = nullptr;
  SegFieldInfo fieldInfo{};
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingOrd;

public:
  GlobalOrdComparator(const std::string& fieldName, std::shared_ptr<OrdMap> ordMap,
                      bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      ordMap(ordMap),
      sortMultiplier(reversed ? -1 : 1) {
    
    // For string fields, ordinal 0 means missing value
    // For MISSING_FIRST: missing values should sort before all other values
    // For MISSING_LAST: missing values should sort after all other values
    // Real ords get the direction multiplier; missing is already placed at
    // the requested edge and must not be reversed a second time.
    if (missingValue == MISSING_FIRST) {
      missingOrd = std::numeric_limits<int64_t>::min();
    } else {
      missingOrd = std::numeric_limits<int64_t>::max();
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* reader_) override {
    reader.reset();
    segmentMapping = {};
    postingsReader = reader_;
    fieldInfo = {};

    if (!postingsReader) return;

    // Get segment-to-global ordinal mapping from OrdMap
    if (ordMap) {
      segmentMapping = ordMap->getSegToGlobal(segment);
    }

    // Load the ordinal column reader for this segment
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      fieldReader.readFieldInfo(fieldInfo);
      if (!ordMap) {
        segmentMapping.numOrds = fieldInfo.nTerms;
      }
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
      }
    }
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > globalOrds.size()) {
      resizeExact(globalOrds, (size_t)capacity);
    }
  }

  // Identity mapping only (ordMap == nullptr, the single-segment selection):
  // slot values are then raw segment ords times the direction, so the bottom
  // slot converts straight back to a segment-ord bound. The multi-segment
  // global mode has no global-to-segment inverse and stays unsupported.
  OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) override {
    if (!reader.has_value() || ordMap != nullptr || postingsReader == nullptr
        || fieldInfo.nTerms == 0) {
      return OrdIntervalStatus::UNSUPPORTED;
    }
    // Missing-first: docs without the field beat any real-valued bottom but
    // have no postings, so a term interval can never represent them.
    if (missingOrd == std::numeric_limits<int64_t>::min()) {
      return OrdIntervalStatus::PENDING;
    }
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t key = globalOrds[(size_t)bottomSlot];
    if (key == missingOrd) return OrdIntervalStatus::PENDING;
    int64_t ord = sortMultiplier < 0 ? -key : key;
    assert(ord >= 1 && ord <= segmentMapping.numOrds);
    // The bottom ord is always exact here (same-segment slot values).
    bool closable = soleSort && segdoc(segment, from) >= bottomDoc;
    if (sortMultiplier > 0) {
      out.lo = 1;
      out.hi = closable ? ord - 1 : ord;
    } else {
      out.lo = closable ? ord + 1 : ord;
      out.hi = segmentMapping.numOrds;
    }
    out.nTerms = segmentMapping.numOrds;
    out.postingsReader = postingsReader;
    out.fieldInfo = &fieldInfo;
    return OrdIntervalStatus::READY;
  }

  int64_t getGlobalOrd(int32_t docid) {
    if (!reader.has_value()) {
      return missingOrd;
    }

    int64_t segmentOrd = selectedSortOrd(*reader, docid, sortMultiplier < 0);
    if (segmentOrd == 0) {
      // Ordinal 0 means missing value
      return missingOrd;
    }
    
    // Convert segment ordinal to global ordinal
    // Segment ords are 1-based while the delta array is 0-based.
    int64_t globalOrd = segmentOrd + segmentMapping.deltaAt(segmentOrd - 1);
    
    // Apply sort multiplier for ascending/descending sort
    return sortMultiplier * globalOrd;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t bottomOrd = globalOrds[bottomSlot];
    int64_t newOrd = getGlobalOrd(newDoc.docId());
    return (bottomOrd > newOrd) - (bottomOrd < newOrd);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    globalOrds[slot] = getGlobalOrd(doc.docId());
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = otherOrd->globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->globalOrds.size());
    globalOrds[slot] = otherOrd->globalOrds[otherSlot];
  }
  
  ~GlobalOrdComparator() override = default;
};

class SegmentOrdComparator : public FieldComparator {
  struct Slot {
    int32_t ord = 0;
    int32_t ordGen = -1;
    char* term = nullptr;
    char* buffer = nullptr;
    uint32_t length = 0;
    uint32_t capacity = 0;
  };

  std::string fieldName;
  MemPool termPool;
  MemPool enumPool;
  MemPool::save_point enumPoolStart;
  SegFieldInfo fieldInfo{};
  std::optional<OrdColReader> reader;
  std::optional<TermsEnum> termsEnum;
  PostingsReader* postingsReader = nullptr;
  std::vector<Slot> slots;
  int32_t currentSegment = -1;
  int32_t bottomOrd = 0;
  int32_t sortMultiplier;
  int32_t missingSortCmp;
  bool missingFirst;
  bool bottomMissing = true;
  bool bottomExact = false;

  static int compareTerms(std::string_view a, std::string_view b) {
    size_t len = std::min(a.size(), b.size());
    int cmp = memcmp(a.data(), b.data(), len);
    if (cmp != 0) return cmp;
    return (a.size() > b.size()) - (a.size() < b.size());
  }

  std::string_view term(const Slot& slot) const {
    assert(slot.ord != 0);
    assert(slot.term != nullptr);
    return {slot.term, slot.length};
  }

  int compareMissing(bool aMissing, bool bMissing) const {
    if (aMissing == bMissing) return 0;
    return aMissing ? missingSortCmp : -missingSortCmp;
  }

  int compareSlots(const Slot& a, const Slot& b) const {
    bool aMissing = a.ord == 0;
    bool bMissing = b.ord == 0;
    if (aMissing || bMissing) return compareMissing(aMissing, bMissing);
    if (a.ordGen == b.ordGen) {
      return (a.ord > b.ord) - (a.ord < b.ord);
    }
    return compareTerms(term(a), term(b));
  }

  void setMissing(Slot& slot) {
    slot.ord = 0;
    slot.ordGen = -1;
    slot.term = nullptr;
    slot.length = 0;
  }

  void copyTerm(Slot& slot, std::string_view value) {
    if (slot.capacity < value.size() || slot.buffer == nullptr) {
      size_t capacity = std::bit_ceil(std::max<size_t>(8, value.size()));
      slot.buffer = termPool.alloc(capacity);
      slot.capacity = (uint32_t)capacity;
    }
    if (!value.empty()) memmove(slot.buffer, value.data(), value.size());
    slot.term = slot.buffer;
    slot.length = (uint32_t)value.size();
  }

  int32_t selectedOrd(int32_t docid) const {
    if (!reader.has_value()) return 0;
    return selectedSortOrd(*reader, docid, sortMultiplier < 0);
  }

public:
  SegmentOrdComparator(const std::string& fieldName,
                       bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      enumPoolStart(enumPool.getSavePoint()),
      sortMultiplier(reversed ? -1 : 1),
      // GlobalOrdComparator keeps its missing sentinel at the requested edge.
      missingSortCmp((missingValue == MISSING_FIRST ? -1 : 1) * sortMultiplier),
      missingFirst(missingValue == MISSING_FIRST) {}

  void setSegment(int32_t segment, PostingsReader* reader_) override {
    termsEnum.reset();
    reader.reset();
    enumPool.rewind(enumPoolStart);
    fieldInfo = {};
    currentSegment = segment;
    postingsReader = reader_;

    if (!postingsReader) return;

    FieldReader fieldReader(*postingsReader);
    if (!fieldReader.seek(fieldName)) return;
    fieldReader.readFieldInfo(fieldInfo);
    if (fieldInfo.nTerms == 0 || fieldInfo.columnLoc.offset() == 0 ||
        fieldInfo.ordFormat == SegFieldInfo::ORD_NONE) {
      return;
    }
    reader.emplace(*postingsReader, fieldInfo);
    termsEnum.emplace(enumPool, *postingsReader, fieldInfo);
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > slots.size()) resizeExact(slots, (size_t)capacity);
  }

  // Bottom state semantics (see setBottom): exact => bottomOrd is the
  // bottom's own 1-based ord, so equal-ord docs tie with the bottom;
  // inexact => the bottom term is absent here and sorts strictly between
  // bottomOrd and bottomOrd + 1, so equal-ord docs strictly beat it and
  // there is no boundary tie to reason about.
  OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) override {
    unused(bottomSlot);
    if (!termsEnum.has_value() || !reader.has_value()) {
      return OrdIntervalStatus::UNSUPPORTED;
    }
    // Missing-first: docs without the field beat any real-valued bottom but
    // have no postings, so a term interval can never represent them.
    if (missingFirst || bottomMissing) return OrdIntervalStatus::PENDING;
    int64_t nTerms = (int64_t)fieldInfo.nTerms;
    bool closable = soleSort && bottomExact
        && segdoc(segment, from) >= bottomDoc;
    if (sortMultiplier > 0) {
      out.lo = 1;
      out.hi = closable ? (int64_t)bottomOrd - 1 : (int64_t)bottomOrd;
    } else {
      out.lo = bottomExact ? (closable ? (int64_t)bottomOrd + 1
                                       : (int64_t)bottomOrd)
                           : (int64_t)bottomOrd + 1;
      out.hi = nTerms;
    }
    out.nTerms = nTerms;
    out.postingsReader = postingsReader;
    out.fieldInfo = &fieldInfo;
    return OrdIntervalStatus::READY;
  }

  void setBottom(int32_t slot) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& bottom = slots[slot];
    bottomMissing = bottom.ord == 0;
    bottomExact = false;
    bottomOrd = 0;
    if (bottomMissing || !termsEnum.has_value()) return;

    if (bottom.ordGen == currentSegment) {
      bottomOrd = bottom.ord;
      bottomExact = true;
      return;
    }

    std::string_view value = term(bottom);
    if (!termsEnum->seekCeil(value)) {
      assert(fieldInfo.nTerms <= INT32_MAX);
      bottomOrd = (int32_t)fieldInfo.nTerms;
      return;
    }
    if (termsEnum->term() == value) {
      bottomOrd = (int32_t)termsEnum->ord() + 1;
      bottomExact = true;
      bottom.ord = bottomOrd;
      bottom.ordGen = currentSegment;
    } else {
      bottomOrd = (int32_t)termsEnum->ord();
    }
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)slots.size());
    return sortMultiplier * compareSlots(slots[slotA], slots[slotB]);
  }

  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc,
                                   segdoc newDoc) override {
    unused(bottomDoc);
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)slots.size());
    assert(bottomMissing == (slots[bottomSlot].ord == 0));
    int32_t docOrd = selectedOrd(newDoc.docId());
    bool docMissing = docOrd == 0;
    int cmp;
    if (bottomMissing || docMissing) {
      cmp = compareMissing(bottomMissing, docMissing);
    } else if (bottomExact) {
      cmp = (bottomOrd > docOrd) - (bottomOrd < docOrd);
    } else {
      cmp = bottomOrd >= docOrd ? 1 : -1;
    }
    return sortMultiplier * cmp;
  }

  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& target = slots[slot];
    int32_t ord = selectedOrd(doc.docId());
    if (ord == 0) {
      setMissing(target);
      return;
    }
    assert(termsEnum.has_value());
    termsEnum->seekOrd(ord - 1);
    copyTerm(target, (std::string_view)termsEnum->term());
    target.ord = ord;
    target.ordGen = currentSegment;
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other,
              int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fieldName == otherOrd->fieldName);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->slots.size());
    return sortMultiplier * compareSlots(slots[slotA], otherOrd->slots[slotB]);
  }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot,
            segdoc otherDoc) override {
    unused(otherDoc);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fieldName == otherOrd->fieldName);
    assert(slot >= 0 && slot < (int32_t)slots.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->slots.size());
    Slot& target = slots[slot];
    const Slot& source = otherOrd->slots[otherSlot];
    if (source.ord == 0) {
      setMissing(target);
      return;
    }
    copyTerm(target, otherOrd->term(source));
    target.ord = source.ord;
    target.ordGen = source.ordGen;
  }
};

// A comparator for non-indexed string columns that compares by value
class StrColComparator : public FieldComparator {
  std::string fieldName;
  std::optional<StrColReader> reader;
  std::optional<StrColReader::Iterator> iter;
  std::vector<std::string> values; // Storage for string values
  int sortMultiplier; // 1 for ascending, -1 for descending
  std::string missingValueStr;
  
public:
  StrColComparator(const std::string& fieldName, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      sortMultiplier(reversed ? -1 : 1) {
    
    // For missing values, use empty string for MISSING_FIRST in ASC (sorts before all others)
    // or a very high value string for MISSING_LAST in ASC
    if (missingValue == MISSING_FIRST) {
      missingValueStr = reversed ? "\xFF\xFF\xFF\xFF" : "";
    } else {
      missingValueStr = reversed ? "" : "\xFF\xFF\xFF\xFF";
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    reader.reset();
    iter.reset();
    
    if (!postingsReader) return;
    
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
      }
    }
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > values.size()) resizeExact(values, (size_t)capacity);
  }

  std::string_view getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return missingValueStr;
    }
    
    // Advance iterator to docid if needed
    if (iter->docId() < docid) {
      int32_t foundDoc = iter->advance(docid);
      if (foundDoc == StrColReader::Iterator::ENDDOC || foundDoc > docid) {
        return missingValueStr;
      }
    } else if (iter->docId() > docid) {
      return missingValueStr;
    }
    
    return iter->value();
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    const auto& valA = values[slotA];
    const auto& valB = values[slotB];
    int cmp = valA.compare(valB);
    return sortMultiplier * cmp;
  }
  
  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    const auto& bottomVal = values[bottomSlot];
    std::string_view newVal = getDocValue(newDoc.docId());
    int cmp = bottomVal.compare(newVal);
    return sortMultiplier * cmp;
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    values[slot] = std::string(getDocValue(doc.docId()));
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherStr->values.size());
    const auto& valA = values[slotA];
    const auto& valB = otherStr->values[slotB];
    int cmp = valA.compare(valB);
    return sortMultiplier * cmp;
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherStr->values.size());
    values[slot] = otherStr->values[otherSlot];
  }
  
  ~StrColComparator() override = default;
};

} // namespace solux
