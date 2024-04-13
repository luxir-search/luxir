
#include <gtest/gtest.h>
#include <iostream>
#include <solux/index/Inverter.h>
#include <latch>

#include "solux/index/IndexWriter.h"
#include "solux/search/IndexReader.h"
#include "test/SoluxTest.h"

using namespace std;
using namespace solux;

class IndexWriterTest : public SoluxTest {
public:
  std::string field = "text_w";

  void addDoc(IndexWriter& iw) {
    auto* inverter = &iw.obtainInverter();
    auto* fieldHandler = &inverter->getIndexHandler(field);
    std::string doc1 = "test";
    inverter->startDoc();
    fieldHandler->index(*inverter, doc1.data(), doc1.size());
    inverter->finishDoc();
    iw.releaseInverter(*inverter);
  }
};


TEST_F(IndexWriterTest, simple) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto* inverter = &iw.obtainInverter();
  auto* fieldHandler = &inverter->getIndexHandler(field);

  std::string doc1 = "now is the time for all good men";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1.data(), doc1.size());
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r1(dir);
  ASSERT_EQ(1, r1.segments().size());
  ASSERT_EQ(1, r1.numDocs());

  inverter = &iw.obtainInverter();
  fieldHandler = &inverter->getIndexHandler(field);

  doc1 = "to come to the aid";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1.data(), doc1.size());
  inverter->finishDoc();
  doc1 = "of their country";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1.data(), doc1.size());
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r2(dir);
  ASSERT_EQ(2, r2.segments().size());
  ASSERT_EQ(3, r2.numDocs());
  ASSERT_GT(r2.commitTime(), r1.commitTime());
}

// Test retrieving IndexReader from the IndexWriter
TEST_F(IndexWriterTest, getReader) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  ASSERT_EQ(0, reader->numDocs());
  ASSERT_EQ(0, reader->segments().size());  // could change depending on impl

  addDoc(iw);

  // no commit, so getIndexReader() should return the same reader
  auto reader2 = iw.getIndexReader();
  ASSERT_EQ(reader, reader2);

  // now make it visible.
  iw.commit();
  reader = iw.getIndexReader();
  ASSERT_EQ(1, reader->numDocs());
  ASSERT_EQ(1, reader->segments().size());

  // now add another doc, commit, but get a reader with permissive freshness
  addDoc(iw);
  iw.commit();
  reader2 = iw.getIndexReader(10000000);  // can be up to 10 seconds old
  ASSERT_EQ(reader, reader2);  // not guaranteed, but should be the case

  // sleep current thread for a microsecond
  std::this_thread::sleep_for(std::chrono::microseconds(1));

  // now test a reader that is not fresh enough
  reader2 = iw.getIndexReader(1);  // can be up to 1 microseconds old! (0 is a special case, so we just chose smallest value we can)
  ASSERT_NE(reader, reader2);  // It's possible this could spuriously fail if the sleep wasn't long enough or the system clock is changed.
  ASSERT_EQ(2, reader2->numDocs());
  ASSERT_EQ(2, reader2->segments().size());
}


// Test automatic merging kick-off
TEST_F(IndexWriterTest, autoMerge) {
  for (int iter=0; iter<100; iter++) {
    RAMDir dir;
    IndexWriter iw(dir);
    int MERGE_FACTOR = 10;

    // add  segments
    for (int i = 0; i < MERGE_FACTOR - 1; i++) {
      addDoc(iw);
      iw.commit();
    }

    // make sure we're making the segments we think we are:
    auto reader = iw.getIndexReader();
    ASSERT_EQ(MERGE_FACTOR - 1, reader->segments().size());
    ASSERT_EQ(MERGE_FACTOR - 1, reader->numDocs());

    addDoc(iw);
    // MERGE_FACTOR segments of the same size will set off an auto-merge.
    // For this reason, we need a callback to try grabbing the new IndexReader before the merge completes.
    // We need to block the merge operation until we've grabbed a new reader.
    std::latch mergeStart(1);
    auto callback = [&mergeStart](void* a, void* b, void* c) -> void* {
      unused(a,b,c);
      mergeStart.wait();
      return nullptr;
    };

    solux::Signal::listen("mergeStart", std::move(callback));

    // this should cause a segment flush and a merge to kick off, but we've blocked the merge from completing until
    // later.
    // NOTE: this deadlocked since the wait() in the commit call work-steals the mergeSegments task (which will
    // only be un-blocked after the commit call completes).
    iw.commit();

    // pre-merge view
    reader = iw.getIndexReader();
    ASSERT_EQ(reader->numDocs(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), MERGE_FACTOR);

    mergeStart.count_down(); // let the merge continue

    // Now wait until the merge completes.
    iw.updateGraph.wait_for_all();

    reader = iw.getIndexReader();
    ASSERT_EQ(reader->numDocs(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), 1);
  }
}