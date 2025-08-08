#pragma once
#include <vector>
#include <span>
#include <solux/util/screaming.h>

using namespace screaming;
namespace solux {

class DocSetBuilder;

class DocSet {
  friend DocSetBuilder;
protected:
  int32_t card_ = -1;
  virtual int32_t calcCard(){ return -1;};

public:
  enum Type {
    ARRAY = 0,
    BITSET = 1
  };
  const Type type;

  DocSet(Type type): type(type) {}

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

  virtual bool get(int32_t docid) const = 0; // returns true if the docid is in the set

  virtual ~DocSet() = default;
};

/// non-owning BitDocSet
/// Used for segment liveDocs.
class BitDocSet : public DocSet {
protected:
  FixedBitSet bits_; // non-owning bitset

public:
  BitDocSet(FixedBitSet bits) : DocSet(BITSET), bits_(bits) {
  }

  BitDocSet(FixedBitSet bits, int32_t card) : DocSet(BITSET), bits_(bits) {
    card_ = card;
  }

  bool get(int32_t docid) const override {
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

  RAMBitDocSet(RAMBitDocSet&& other) noexcept : BitDocSet(std::move(other.bits_)) {
    card_ = other.card_;
    other.bits_.words = nullptr;
    other.card_ = -1; // invalidate the moved-from object
  }
/*
  RAMBitDocSet& operator=(RAMBitDocSet&& other) noexcept {
    if (this != &other) {
      bits_ = other.bits_;
      bits_.words = other.bits_.words;
      card_ = other.card_;
      other.bits_.words = nullptr; // invalidate the moved-from object
      other.card_ = -1;
    }
    return *this;
  }
  */

  ~RAMBitDocSet() override {
    delete[] bits_.words; // free the allocated memory
  }
};

/// Sorted array of docs.
class ArrDocSet : public DocSet {
protected:
  std::vector<int32_t> docs_;

  int32_t calcCard() override {
    return static_cast<int32_t>(docs_.size());
  }

public:
  ArrDocSet(std::vector<int32_t>&& docs) : DocSet(ARRAY), docs_(std::move(docs)) {
    card_ = static_cast<int32_t>(docs_.size());
  }

  std::span<int32_t> docs() {
    return docs_;
  }

  bool get(int32_t docid) const override {
    // do a binary search for the docid
    return std::binary_search(docs_.begin(), docs_.end(), docid);
  }

};

class DocSetBuilder {
public:
  int32_t max;
  std::vector<int32_t> docs;
  std::optional<RAMBitDocSet> bitDocs;
  DocSetBuilder(int32_t max) : max(max) {
    docs.reserve(max);
  }

  void add(int32_t docid) {
    if (bitDocs.has_value()) {
      bitDocs->mutableBits().set(docid);
      bitDocs->card_++;
      return;
    }
    if (docs.size() * 32 < max) {
      docs.emplace_back(docid);
      return;
    }
    bitDocs.emplace(max);
    for (auto d : docs) {
      bitDocs->mutableBits().set(d);
    }
    bitDocs->mutableBits().set(docid);
    bitDocs->setCard(docs.size() + 1);
    docs.clear();
    docs.shrink_to_fit();
  }

  std::unique_ptr<DocSet> build() {
    if (bitDocs.has_value()) {
      return std::make_unique<RAMBitDocSet>(std::move(*bitDocs));
    }
    docs.shrink_to_fit();
    return std::make_unique<ArrDocSet>(std::move(docs));
  }
};


}
