// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/DomainIter.h"

#include <bit>
#include <stdexcept>

namespace luxir {

namespace {

constexpr int32_t ORD_BLOCK = (int32_t)OrdColReader::BULK_SIZE;
constexpr int32_t NO_DOC = OrdColReader::ENDDOC;
// An empty batch always takes a whole block, so every fill makes progress.
static_assert(OrdValueBatches::BATCH_SIZE >= ORD_BLOCK);

} // namespace

OrdValueBatches::OrdValueBatches(DocSet* domain, const OrdColReader& column,
                                 const OrdMap::SegToGlobal& ordSpace,
                                 int32_t maxDoc, bool complement,
                                 OrdColReader::ForEachOrdStats* stats)
    : column(column), ordSpace(ordSpace), stats(stats), maxDoc(maxDoc) {
  if (!complement && domain != nullptr
      && domain->type == DocSet::Type::ARRAY) {
    auto* arrayDomain = dynamic_cast<ArrDocSet*>(domain);
    if (arrayDomain == nullptr) {
      throw std::logic_error("ARRAY DocSet has a non-array dynamic type");
    }
    std::span<int32_t> docs = arrayDomain->docs();
    arrayNext = docs.data();
    arrayEnd = docs.data() + docs.size();
  } else if (domain != nullptr) {
    bits = &((BitDocSet*)domain)->bits();
  }
  // A null domain's complement is empty, and array domains expand their
  // complement words first (forEachComplementOrdValue).
  assert(!complement || bits != nullptr);

  if (!column.docIdIndexed()) {
    walk = Walk::RANK;
    source = arrayNext != nullptr ? Source::ARRAY
        : complement ? Source::COMPLEMENT
        : bits != nullptr ? Source::BITS
                          : Source::ALL;
    rankIter.emplace(column);
    return;
  }
  assert(!column.multiValued());
  assert(arrayNext != nullptr || maxDoc == column.encodedValues());
  if (arrayNext != nullptr) {
    walk = Walk::DOCID_ARRAY;
  } else if (bits != nullptr) {
    assert(bits->size() >= maxDoc);
    walk = Walk::DOCID_BITS;
    invertMask = complement ? ~0ULL : 0;
    bulkMinHits = OrdColReader::configuredBulkMinHits();
  } else {
    walk = Walk::DOCID_ALL;
  }
}

bool OrdValueBatches::next(Batch& batch) {
  batch.size = 0;
  switch (walk) {
    case Walk::DOCID_ARRAY: fillDocIdArray(batch); break;
    case Walk::DOCID_BITS: fillDocIdBits(batch); break;
    case Walk::DOCID_ALL: fillDocIdAll(batch); break;
    case Walk::RANK:
      switch (source) {
        case Source::ARRAY: fillRank<Source::ARRAY>(batch); break;
        case Source::BITS: fillRank<Source::BITS>(batch); break;
        case Source::COMPLEMENT: fillRank<Source::COMPLEMENT>(batch); break;
        case Source::ALL: fillRank<Source::ALL>(batch); break;
      }
      break;
  }
  ordSpace.globalOrdsOfStored(batch.stored, batch.size, batch.ords);
  return batch.size > 0;
}

// Point decodes, a batch's worth of domain docs at a time.
void OrdValueBatches::fillDocIdArray(Batch& batch) {
  while (arrayNext != arrayEnd && batch.size < BATCH_SIZE) {
    int32_t take = (int32_t)std::min<int64_t>(BATCH_SIZE - batch.size,
                                              arrayEnd - arrayNext);
    int32_t present = column.presentOrdsAt(
        std::span<const int32_t>(arrayNext, (size_t)take),
        batch.docs + batch.size, batch.stored + batch.size);
    arrayNext += take;
    batch.size += present;
    missingCount += take - present;
    if (stats) stats->pointLoads += take;
  }
}

// One 128-doc block at a time, each choosing point decodes or one bulk
// unpack by its own hits. invertMask XORs each domain word before use, so the
// same walk serves a domain and its complement; bits past maxDoc are masked
// after the inversion, so inverted garbage is never visited.
void OrdValueBatches::fillDocIdBits(Batch& batch) {
  auto windowWord = [&](int32_t word, int32_t end) LUXIR_INLINE {
    uint64_t wordBits = bits->words[word] ^ invertMask;
    int32_t validBits = end - (word << 6);
    if (validBits < 64) wordBits &= (1ULL << validBits) - 1;
    return wordBits;
  };
  while (cursor < maxDoc) {
    int32_t start = cursor;
    int32_t end = std::min(start + ORD_BLOCK, maxDoc);
    int32_t word = start >> 6;
    uint64_t low = windowWord(word, end);
    uint64_t high = start + 64 < end ? windowWord(word + 1, end) : 0;
    int32_t hits = std::popcount(low) + std::popcount(high);
    // A block joins the batch while its hits fit, so batches fill by hits
    // rather than by the worst case of a full block.
    if (batch.size + hits > BATCH_SIZE) break;
    cursor = end;
    if (hits == 0) continue;

    int32_t* docsOut = batch.docs + batch.size;
    int32_t* storedOut = batch.stored + batch.size;
    int32_t present = 0;
    bool bulk = OrdColReader::bulkBlock(hits, end - start, bulkMinHits);
    if (bulk) {
      uint32_t frame[ORD_BLOCK];
      column.unpack(start, (uint32_t)(end - start), frame);
      for (int32_t half = 0; half < 2; half++) {
        uint64_t wordBits = half == 0 ? low : high;
        while (wordBits != 0) {
          int32_t offset = half * 64 + std::countr_zero(wordBits);
          wordBits &= wordBits - 1;
          uint32_t ord = frame[offset];
          assert(ord <= INT32_MAX);
          docsOut[present] = start + offset;
          storedOut[present] = (int32_t)ord;
          present += ord != 0;
        }
      }
    } else {
      uint64_t words[2] = {low, high};
      present = column.presentOrdsAtBits(words, start, docsOut, storedOut);
    }
    batch.size += present;
    missingCount += hits - present;
    if (stats) {
      if (bulk) {
        stats->bulkLoads += hits;
        stats->bulkBlocks++;
      } else {
        stats->pointLoads += hits;
        stats->pointBlocks++;
      }
    }
  }
}

// Every doc, in bulk frames.
void OrdValueBatches::fillDocIdAll(Batch& batch) {
  while (cursor < maxDoc && batch.size + ORD_BLOCK <= BATCH_SIZE) {
    int32_t start = cursor;
    int32_t count = std::min(ORD_BLOCK, maxDoc - start);
    cursor += count;
    uint32_t frame[ORD_BLOCK];
    column.unpack(start, (uint32_t)count, frame);
    int32_t present = batch.size;
    for (int32_t i = 0; i < count; i++) {
      uint32_t ord = frame[i];
      assert(ord <= INT32_MAX);
      batch.docs[present] = start + i;
      batch.stored[present] = (int32_t)ord;
      present += ord != 0;
    }
    missingCount += count - (present - batch.size);
    batch.size = present;
    if (stats) stats->bulkLoads += count;
  }
}

template <OrdValueBatches::Source S>
int32_t OrdValueBatches::nextRankDoc() {
  if constexpr (S == Source::ARRAY) {
    return arrayNext == arrayEnd ? NO_DOC : *arrayNext++;
  } else if constexpr (S == Source::ALL) {
    return cursor < maxDoc ? cursor++ : NO_DOC;
  } else {
    // Never ask nextSetBit() to start at maxDoc when the last doc is set.
    if (cursor >= maxDoc) return NO_DOC;
    int32_t doc = S == Source::BITS ? (int32_t)bits->nextSetBit(cursor)
                                    : (int32_t)bits->nextClearBit(cursor);
    if (doc >= maxDoc) {
      cursor = maxDoc;
      return NO_DOC;
    }
    cursor = doc + 1;
    return doc;
  }
}

// Per domain doc: its rank among the docs with the field, then its values.
template <OrdValueBatches::Source S>
void OrdValueBatches::fillRank(Batch& batch) {
  constexpr bool point = S == Source::ARRAY;
  OrdColReader::Iterator& iter = *rankIter;
  OrdColReader::PointOrds pointOrds(column);
  auto value = [&](int64_t rank) LUXIR_INLINE {
    if (stats) {
      if (point) stats->pointLoads++;
      else stats->bulkLoads++;
    }
    return point ? pointOrds.valueAt(rank) : iter.values().valueAt(rank);
  };
  auto drainPending = [&]() LUXIR_INLINE {
    for (; pendingRank < pendingEnd && batch.size < BATCH_SIZE; pendingRank++) {
      batch.docs[batch.size] = pendingDoc;
      batch.stored[batch.size] = value(pendingRank);
      batch.size++;
    }
  };

  drainPending();
  while (batch.size < BATCH_SIZE) {
    int32_t doc = nextRankDoc<S>();
    if (doc == NO_DOC) break;
    if (iter.docId() < doc) iter.advance(doc);
    if (iter.docId() != doc) {
      missingCount++;
      continue;
    }
    if (!column.multiValued()) {
      batch.docs[batch.size] = doc;
      batch.stored[batch.size] = value(iter.rank());
      batch.size++;
      continue;
    }
    auto [start, end] = column.getStartEndValueRank(iter.rank());
    pendingDoc = doc;
    pendingRank = start;
    pendingEnd = end;
    drainPending();
  }
}

bool DistinctOrdValueBatches::next(OrdValueBatches::Batch& batch) {
  while (values.next(batch)) {
    int32_t out = 0;
    for (int32_t i = 0; i < batch.size; i++) {
      if (batch.docs[i] != lastDoc) {
        seen.clear();
        lastDoc = batch.docs[i];
      }
      if (!seen.insert(batch.ords[i]).second) continue;
      batch.docs[out] = batch.docs[i];
      batch.ords[out] = batch.ords[i];
      out++;
    }
    batch.size = out;
    if (out != 0) return true;
  }
  return false;
}

} // namespace luxir
