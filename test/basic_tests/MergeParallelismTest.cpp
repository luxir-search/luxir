#include <algorithm>
#include <gtest/gtest.h>

#include "luxir/index/MergeCostModel.h"
#include "luxir/reader/Postings.h"
#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class BudgetTotalGuard {
  IndexRamBudget& budget;
  int64_t oldTotal;

public:
  explicit BudgetTotalGuard(IndexRamBudget& budget)
    : budget(budget), oldTotal(budget.totalBytes()) {}

  ~BudgetTotalGuard() {
    budget.setTotalBytes(oldTotal);
  }
};

std::vector<Doc> mergeCorpus() {
  std::vector<Doc> docs;
  docs.reserve(24);
  for (int i = 0; i < 24; i++) {
    std::string id = std::to_string(i);
    std::string title = std::string("alpha beta ") + (i % 2 == 0 ? "even" : "odd");
    std::string cat = std::string("cat") + std::to_string(i % 3);
    std::string raw = std::string("raw-") + std::to_string(i % 5);
    std::string tag = std::string("tag") + std::to_string(i % 4);
    docs.push_back(flatdoc("id", id,
                           "title_t", title,
                           "cat_s", cat,
                           "tags_ss", vecs(tag, "common"),
                           "price_i", (int64_t)(i * 7),
                           "nums_is", vec_i(i, i + 10),
                           "raw_sc", raw));
  }
  return docs;
}

std::string docId(const Doc& doc) {
  auto* id = find(doc, "id");
  assert(id != nullptr);
  return std::get<std::string>(*id);
}

void sortById(std::vector<Doc>& docs) {
  std::sort(docs.begin(), docs.end(), [](const Doc& a, const Doc& b) {
    return docId(a) < docId(b);
  });
}

void buildMergedIndex(CollectionHelper& helper) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(1000);
  auto docs = mergeCorpus();
  for (const auto& doc : docs) {
    auto result = helper.index(doc, UpdateMessage::COMMIT, true);
    ASSERT_TRUE(result.success);
  }
  iw->mergeSegments();
}

std::vector<Doc> allMergedDocs(CollectionHelper& helper) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main")
      .topDocs("q")
      .allQuery()
      .fields({"id", "title_t", "cat_s", "tags_ss", "price_i", "nums_is", "raw_sc"})
      .limit(-1)
      .getNumber();
  req->execute();
  EXPECT_TRUE(req->ok()) << req->toString();
  auto docs = req->getDocs();
  sortById(docs);
  return docs;
}

int64_t countTitleMatches(CollectionHelper& helper, std::string_view term) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("title_t", term).getNumber();
  req->execute();
  EXPECT_TRUE(req->ok()) << req->toString();
  return req->getMatchCount();
}

void expectSameDocs(const std::vector<Doc>& expected, const std::vector<Doc>& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_TRUE(docEquals(expected[i], actual[i]))
      << "expected: " << docToString(expected[i]) << "\nactual: " << docToString(actual[i]);
  }
}

int mergedSegmentFileCount(CollectionHelper& helper) {
  auto iw = helper.getIndexWriter();
  auto reader = iw->getIndexReader();
  assert(reader->segments().size() == 1);
  uint64_t segId = reader->segments()[0].segInfo.seg_id;
  std::string prefix = Postings::getIndexFileNamePrefix(segId);

  std::vector<std::string> files;
  iw->dir.listFiles(files);
  int count = 0;
  for (const auto& file : files) {
    if (file.starts_with(prefix)) {
      count++;
    }
  }
  return count;
}

} // namespace

TEST(MergeParallelismTest, SerialAndParallelMergedContentEquivalent) {
  auto& budget = LuxirTest::luxirNode->getIndexRamBudget();
  BudgetTotalGuard guard(budget);
  CollectionHelper helper("main");

  budget.setTotalBytes(1);
  buildMergedIndex(helper);
  auto serialDocs = allMergedDocs(helper);
  int64_t serialOdd = countTitleMatches(helper, "odd");
  int64_t serialEven = countTitleMatches(helper, "even");

  budget.setTotalBytes(0);
  buildMergedIndex(helper);
  auto parallelDocs = allMergedDocs(helper);
  int64_t parallelOdd = countTitleMatches(helper, "odd");
  int64_t parallelEven = countTitleMatches(helper, "even");

  expectSameDocs(serialDocs, parallelDocs);
  EXPECT_EQ(serialOdd, parallelOdd);
  EXPECT_EQ(serialEven, parallelEven);
}

TEST(MergeParallelismTest, MergedSegmentFileCountStaysWithinStreamCap) {
  auto& budget = LuxirTest::luxirNode->getIndexRamBudget();
  BudgetTotalGuard guard(budget);
  budget.setTotalBytes(0);

  CollectionHelper helper("main");
  buildMergedIndex(helper);

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1, reader->segments().size());
  EXPECT_LE(mergedSegmentFileCount(helper), MergeCostModel::MAX_STREAMS);
}
