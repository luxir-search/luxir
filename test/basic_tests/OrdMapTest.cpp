#include <gtest/gtest.h>
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "test/SoluxTest.h"
#include "solux/search/OrdMap.h"
// #include "solux/search/OrdMapImpl.h"
#include "solux/search/IndexReader.h"
#include "solux/index/UpdateMessage.h"

using namespace solux;
using namespace solux::test;

class OrdMapTest : public ::testing::Test {
protected:
  void SetUp() override {
    helper = std::make_unique<CollectionHelper>();
  }

  void TearDown() override {
    helper.reset();
  }

  std::unique_ptr<CollectionHelper> helper;
};
