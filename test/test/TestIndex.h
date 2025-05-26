#pragma once
#include <gtest/gtest.h>
#include "solux/util/solux_util.h"
#include "solux/util/random.h"
#include "solux/index/Inverter.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "test/SoluxTest.h"
#include <vector>


namespace solux::test {

  class ValGen {
  public:
    virtual void init() = 0; // reset value stream
    virtual std::optional<int64_t> intVal() {
      return 0;
    }
    virtual ~ValGen() = default;
  };

  class IntSeq : public ValGen {
  protected:
    const int64_t start;
    const int64_t numValues;
    int64_t v;
  public:
    IntSeq(int64_t start, int64_t numValues) : start(start), numValues(numValues), v(start) {
    }

    void init() override {
      v=start;
    }

    std::optional<int64_t> intVal() override {
      return (*this)();
    }

    std::optional<int64_t> operator()() {
      if (v >= start + numValues) {
        v = start;
        return {};
      }
      return v++;
    }
  };


  class RandGen : public ValGen {
  protected:
    Rng rng;
    uint64_t seed;

  public:
    explicit RandGen(uint64_t seed) : seed(seed) {
      rng.init(seed);
    }
    void init() override {
      rng.init(seed);
    }
  };

  class RandomInts : public RandGen {
  protected:
    int64_t numValues;
    int64_t n = 0;
  public:
    RandomInts(uint64_t seed, int64_t numValues) : RandGen(seed), numValues(numValues) {
    }

    void init() override {
      n = 0;
      RandGen::init();
    }

    std::optional<int64_t> intVal() override {
      return (*this)();
    }

    int64_t val() {
      return (int64_t)rng();
    }

    std::optional<int64_t> operator()() {
      if (n == numValues) {
        init();
        return {};
      }
      n++;
      return val();
    }
  };

  class IncreasingRandomInts : public RandGen {
  protected:
    int64_t numValues;
    int64_t n = 0;
    int64_t v;
    int64_t min;
    int64_t max;
    int64_t maxgap;
  public:
    IncreasingRandomInts(uint64_t seed, int64_t numValues, int64_t min, int64_t max, int64_t maxgap) : RandGen(seed), numValues(numValues), min(min), max(max), maxgap(maxgap) {
      v = min;
    }

    void init() override {
      n = 0;
      v = min;
      RandGen::init();
    }

    std::optional<int64_t> intVal() override {
      return (*this)();
    }

    int64_t val() {
      return v += rng.rint(maxgap) + 1;
    }

    std::optional<int64_t> operator()() {
      if (n == numValues) {
        init();
        return {};
      }
      n++;
      val();
      if (v >= max) {
        init();
        return {};
      }
      return v;
    }
  };


// The TestIndex class is for testing low-level indexing functionality.  It was first introduced before there were
// any higher-level features.  It also acts as a compatibility layer between low level tests
// and actual solux APIs as they change over time.
  class TestIndex {
  public:
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<IndexWriter> iw;
    Inverter* inverter = nullptr;
    MemPool::save_point save = pool.getSavePoint();
    std::string gen;

    std::shared_ptr<IndexReader> reader;

    void clear() {
      pool.rewind(save);
      dir = RAMDir();
    }

    void initWriter() {
      if (iw.get() == nullptr) {
        iw = std::make_unique<IndexWriter>(dir);
      }
      inverter = &iw->obtainInverter();
      gen = inverter->getPostingsWriter().getSegId();
    }

    void flush() {
      if (inverter == nullptr) return;
      iw->releaseInverter(*inverter);
      iw->commit();
      inverter = nullptr;
    }

    Inverter& getInverter() {
      if (inverter == nullptr) {
        initWriter();
      }
      return *inverter;
    }

    Inverter::IndexHandler& getIndexHandler(std::string_view name) {
      return getInverter().getIndexHandler(name);
    }

    void initReader() {
      // auto indexReader = iw->getIndexReader();
      reader = std::make_shared<IndexReader>(dir);
    }


  };

  class TestField {
  public:
    TestIndex& testIndex;
    std::string name;
    Inverter* inverter = nullptr;
    Inverter::IndexHandler* indexHandler = nullptr;

    // segment-level reading
    int currSeg = -1;
    SegFieldInfo fieldInfo;
    std::unique_ptr<IntColReader> colReader;
    std::unique_ptr<IntColReader::Iterator> iter;
    std::unique_ptr<TermsEnum> tenum;



    // TODO: we should separate the "has value" from int column... it can be shared across all fields!

    int32_t nAdds = 0;
    int64_t doc = -1;
    int64_t v = 0;



// TestField is a generic field that can write and read from the TestIndex, and iterate over values of all segments.
// One must use the correct methods depending on the type of the field to get sane results.
    TestField(TestIndex& testIndex, const std::string_view name) : testIndex(testIndex), name(name) {
    }

    void startIndexing() {
      inverter = &testIndex.getInverter();
      indexHandler = &testIndex.getIndexHandler(name);
    }

    void add(int32_t docid, int64_t val) {
      inverter->setDoc(docid);
      indexHandler->index(*inverter, val);
      nAdds++;
    }

    void add(int32_t docid, std::span<const int64_t> vals) {
      inverter->setDoc(docid);
      indexHandler->index(*inverter, vals);
      nAdds++;
    }

    void add(int32_t docid, std::string_view val) {
      inverter->setDoc(docid);
      indexHandler->index(*inverter, val);
      nAdds++;
    }

    void addStrings(int32_t docid, std::vector<std::string_view>&& vals) {
      inverter->setDoc(docid);
      indexHandler->index(*inverter, vals);
      nAdds++;
    }

    void startReading() {
      testIndex.initReader();  // TODO: don't do this for each field, it will invalidate previous pointers!
      currSeg = -1;
      iter.reset();
    }

    int64_t nextDoc() {
      if (iter == nullptr) {
        auto found = nextSegment();
        if (!found) return -1; // OR BIG_END. 0x7ffffffff?
      }
      for(;;) {
        doc = iter->next();
        if (doc != IntColReader::ENDDOC) {
          return doc;
        }
        auto found = nextSegment();
        if (!found) return -1; // OR BIG_END. 0x7ffffffff?
      }
    }

    int64_t val() {
      return v = iter->value();
    }

    int64_t ord() {
      return v = iter->value();
    }

    void vals(std::vector<int64_t>& target) {
      target.resize(0);
      if (!colReader->multiValued()) {
        target.push_back(val());
      } else {
        auto docrank = iter->rank();
        auto [startRank, endRank] = colReader->getStartEndRank(docrank);
        for (auto vrank = startRank; vrank < endRank; vrank++) {
          target.push_back(iter->values().valueAt(vrank));
        }
      }
    }

    void ords(std::vector<int64_t>& target) {
      vals(target);
    }

    bool nextSegment() {
      auto segments = testIndex.reader->segments();

      // do in a loop to skip segments without the field.
      for (;;) {
        if (currSeg + 1 >= (int)segments.size()) {
          return false;
        }
        currSeg++;

        IndexReader::Segment &seg = segments[currSeg];
        FieldReader fieldReader(testIndex.pool, seg.postingsReader());
        auto found = fieldReader.seek(name);
        if (!found) continue;

        fieldReader.readFieldInfo(fieldInfo);
        colReader = std::make_unique<IntColReader>(testIndex.pool, seg.postingsReader(), fieldInfo); // todo nocommit, when will pool rollback be done?
        // EXPECT_EQ(colReader->docsWithField(), nAdds); // TODO: sum up and only do after final segment has been reached
        iter = std::make_unique<IntColReader::Iterator>(*colReader);
        return true;
      }
    }

    IndexReader::Segment* currentSegment() {
      if (currSeg < 0) { nextSegment(); }
      if (testIndex.reader == nullptr || currSeg >= (int)testIndex.reader->segments().size()) {
        LOG_ERROR("TestField {} not reading any segment", name);
        return nullptr;
      }
      return &testIndex.reader->segments()[currSeg];
    }

    TermsEnum createTermsEnum() {
      if (currSeg < 0) { nextSegment(); }
      return TermsEnum(testIndex.pool, currentSegment()->postingsReader(), fieldInfo);
    }
    DocsEnum createDocsEnum(TermsEnum& termsEnum) {
      if (currSeg < 0) { nextSegment(); }
      return DocsEnum(testIndex.pool, currentSegment()->postingsReader(), termsEnum);
    }

    // the format of the array is [docid, termfreq, pos1, pos2, ..., docid2, termfreq2, ...]
    std::vector<int32_t>& readDocsAndPositions(std::vector<int32_t>& target, TermsEnum& termsEnum) {
      DocsEnum docsEnum = createDocsEnum(termsEnum);
      target.resize(0);
      auto numDocs = 0;
      for (;;) {
        auto docid = docsEnum.nextDoc();
        if (docid == DocsEnum::END) break;
        numDocs++;
        target.push_back(docid);
        target.push_back(docsEnum.termFreq());
        docsEnum.startPositions();
        for (int32_t i = 0; i < docsEnum.termFreq(); i++) {
          target.push_back(docsEnum.nextPosition());
        }
        EXPECT_EQ(docsEnum.nextPosition(), DocsEnum::END);
      }
      EXPECT_EQ(numDocs, docsEnum.numDocs());
      return target;
    }

    std::vector<int32_t> readDocsAndPositions(TermsEnum& termsEnum) {
      std::vector<int32_t> target;
      return readDocsAndPositions(target, termsEnum);
    }

    std::vector<int32_t>& readDocsAndPositions(std::vector<int32_t>& target, DocsEnum& docsEnum) {
      target.resize(0);
      int32_t docid = docsEnum.nextDoc();
      if (docid == DocsEnum::END) {
        return target;
      }
      target.push_back(docid);
      docsEnum.startPositions();
      for(;;) {
        int32_t pos = docsEnum.nextPosition();
        if (pos != DocsEnum::END) {
          target.push_back(pos);
        }
      }
      return target;
    }

  };

  struct FieldAndValues {
    TestField testField;
    std::unique_ptr<ValGen> ids;
    std::unique_ptr<ValGen> values;
  };

} // end namespace
