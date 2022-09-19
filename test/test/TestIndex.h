#pragma once
#include <gtest/gtest.h>
#include "solux/util/solux_util.h"
#include "solux/util/random.h"
#include "solux/index/Inverter.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
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


  class TestIndex {
  public:
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<IndexWriter> iw;
    Inverter* inverter = nullptr;
    MemPool::save_point save = pool.getSavePoint();
    std::unique_ptr<PostingsWriter> postingsWriter;
    std::string gen;

    std::unique_ptr<PostingsReader> postingsReader;
    std::unique_ptr<FieldReader> fieldReader;

    void clear() {
      pool.rewind(save);
      dir = RAMDir();
    }

    void initWriter() {
      iw = std::make_unique<IndexWriter>(dir);
      inverter = &iw->getInverter();
      gen = inverter->getPostingsWriter().getSegId();
    }

    void flush() {
      if (inverter == nullptr) return;
      iw->flush();
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
      postingsReader = std::make_unique<PostingsReader>(dir, gen);
      fieldReader = std::make_unique<FieldReader>(pool, *postingsReader);
    }


  };

  class TestField {
  public:
    TestIndex& testIndex;
    std::string name;
    Inverter* inverter = nullptr;
    Inverter::IndexHandler* indexHandler = nullptr;

    SegFieldInfo fieldInfo;
    std::unique_ptr<IntColReader> colReader;
    std::unique_ptr<IntColReader::Iterator> iter;

    int32_t nAdds = 0;
    int32_t doc = -1;
    int64_t v = 0;

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
      // std::cout << "ADDED " << name << " doc=" << doc << " val=" << val << std::endl;
    }

    void startReading() {
      if (testIndex.postingsReader == nullptr) { testIndex.initReader(); }
      // position fieldReader
      EXPECT_EQ(true, testIndex.fieldReader->seek(name));
      testIndex.fieldReader->readFieldInfo(fieldInfo);
      colReader = std::make_unique<IntColReader>(testIndex.pool, *testIndex.postingsReader, fieldInfo);
      ASSERT_EQ(colReader->docsWithValue(), nAdds);
      iter = std::make_unique<IntColReader::Iterator>(*colReader);
    }

    int32_t nextDoc() {
      return doc = iter->next();
    }

    int64_t val() {
      return v = iter->value();
    }
  };

  struct FieldAndValues {
    TestField testField;
    std::unique_ptr<ValGen> ids;
    std::unique_ptr<ValGen> values;
  };

} // end namespace
