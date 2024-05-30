#include "test/SoluxTest.h"
#include "solux/util/MemPool.h"
#include "solux/index/OrdCollector.h"

using namespace std;
using namespace solux;

class OrdCollectorTest : public solux::SoluxTest {
protected:
};


TEST_F(OrdCollectorTest, basic) {
  MemPool pool;
  OrdCollector oc(pool, 5);
  oc.add(1,7);
  oc.add(2, 11);
  oc.add(1, 13);
  oc.add(4, 17);
  oc.add(1, 19);

  ASSERT_FALSE(oc.hasValues(0));
  ASSERT_TRUE(oc.hasValues(1));
  ASSERT_TRUE(oc.hasValues(2));
  ASSERT_FALSE(oc.hasValues(3));
  ASSERT_TRUE(oc.hasValues(4));

  vector<int32_t> ords;
  oc.pushValues(1, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{7, 13, 19}));
  ords.clear();
  oc.pushValues(2, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{11}));
  ords.clear();
  oc.pushValues(3, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{}));
  ords.clear();
  oc.pushValues(4, [&](int32_t ord) { ords.push_back(ord); });
  ASSERT_EQ(ords, (std::vector<int32_t>{17}));
}
