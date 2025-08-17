#pragma once
#include <vector>
#include <span>
#include <solux/util/screaming.h>
#include <solux/util/solux_util.h>

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

  static std::unique_ptr<DocSet> intersect(std::span<DocSet*> sets);

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
  }

  RAMBitDocSet& operator=(RAMBitDocSet&& other) = delete;

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
  const int32_t max;
  std::vector<int32_t> docs;
  std::optional<RAMBitDocSet> bitDocs;
  FixedBitSet* bits = nullptr;
  DocSetBuilder(int32_t max) : max(max) {
    //docs.reserve(max/32);
  }

  void add(int32_t docid) SOLUX_INLINE {
    if (bits) {
      bits->set(docid);
      bitDocs->card_++;
      return;
    }
    if (docs.size() * 32 < max) {
      docs.emplace_back(docid);
      return;
    }
    bitDocs.emplace(max);
    bits = &bitDocs->mutableBits();
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

/// Merge two or more DocSets into a single new DocSet.
inline std::unique_ptr<DocSet> DocSet::intersect(std::span<DocSet*> sets) {
  assert(sets.size() > 1);
  std::sort(sets.begin(), sets.end(), [](DocSet* a, DocSet* b) {
    return a->card() < b->card();
  });
  if (sets[0]->type == BITSET) {
    auto& firstBits = ((BitDocSet*) sets[0])->bits();
    RAMFixedBitSet result(firstBits.size());
    auto nWords = firstBits.sizeInWords(firstBits.size());

    memcpy(result.words, firstBits.words, nWords * sizeof(*result.words));
    for (int i = 1; i < sets.size(); i++) {
      assert(sets[i]->type == BITSET);
      for (int64_t word = 0; word < nWords; word++) {
        result.words[word] &= ((BitDocSet*)sets[i])->bits().words[word];
      }
    }
    return std::make_unique<RAMBitDocSet>(std::move(result));
  }
  // if we get here, we have an array of docids.
  size_t firstbitset = sets.size();
  for (auto i = 0u; i < sets.size(); i++) {
    if (sets[i]->type == BITSET) {
      firstbitset = i;
      break;
    }
  }
  std::vector<int32_t> docStore1;
  std::vector<int32_t> docStore2;
  std::span<int32_t> firstArr = ((ArrDocSet*)sets[0])->docs();
  std::vector<int32_t>* outputDocs = &docStore1;
  std::vector<int32_t>* inputDocs = &docStore2;
  for (auto doc : firstArr) {
    bool missing = false;
    for (int setid = firstbitset; setid < sets.size(); setid++) {
      if (!((BitDocSet*)sets[setid])->bits().get(doc)) {
        missing = true;
        break;
      }
    }
    if (!missing) {
      outputDocs->emplace_back(doc);
    }
  }

  for (int setid = 1; setid < firstbitset; setid++) {
    std::swap(outputDocs, inputDocs);
    outputDocs->clear();
    std::span<int32_t> idocs = *inputDocs;
    std::span<int32_t> jdocs = ((ArrDocSet*)sets[setid])->docs();
    std::set_intersection(idocs.begin(), idocs.end(),
      jdocs.begin(), jdocs.end(),
      std::back_inserter(*outputDocs));
  }
  outputDocs->shrink_to_fit();
  return std::make_unique<ArrDocSet>(std::move(*outputDocs));
}


}
