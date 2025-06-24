#pragma once
#include <vector>
#include <span>
#include <solux/util/screaming.h>

using namespace screaming;
namespace solux {

class DocSet {
protected:
  int32_t card_ = -1;
  virtual int32_t calcCard(){ return -1;};

public:
  /// returns the cardinality, which may involve calculating it first if it's not already known.
  int32_t card() {
    if (card_ == -1) {
      card_ = calcCard();
    }
    return card_;
  }

  /// returns either -1 for unknown, or the cached cardinality.
  int32_t cachedCard() const {
    return card_;
  }

  void setCard(int32_t card) {
    card_ = card;
  }

  virtual ~DocSet() = default;
};

/// All bits are set (contains all docs).
class TrueDocSet : public DocSet {
public:
  TrueDocSet(int32_t size) {
    card_ = size; // all docs are present
  }
};


/// non-owning BitDocSet
/// Used for segment liveDocs.
class BitDocSet : public DocSet {
protected:
  FixedBitSet bits_; // non-owning bitset

public:
  BitDocSet(FixedBitSet bits) : bits_(bits) {
  }

  BitDocSet(FixedBitSet bits, int32_t card) : bits_(bits) {
    card_ = card;
  }

  bool get(int32_t docid) const {
    return bits_.get(docid);
  }

  const FixedBitSet& bits() const {
    return bits_;
  };

  // temporary... remove this in the future.
  FixedBitSet& mutableBits() {
    return bits_;
  }
};


/// BitDocSet owning the DocSet memory.
class RAMBitDocSet : public BitDocSet {
public:
  RAMBitDocSet(int32_t size) : BitDocSet(FixedBitSet(FixedBitSet::allocate(size, false).release(), size)) {}

  RAMBitDocSet(FixedBitSet bits) : BitDocSet(bits) {}

  ~RAMBitDocSet() override {
    delete[] bits_.words; // free the allocated memory
  }
};

/// Sorted array of docs.
class ArrDocSet : public DocSet {
protected:
  std::vector<int32_t> docs_;
public:
  ArrDocSet(std::vector<int32_t> docs) : docs_(std::move(docs)) {
    card_ = static_cast<int32_t>(docs_.size());
  }
  std::span<int32_t> docs() {
    return docs_;
  }
};


}
