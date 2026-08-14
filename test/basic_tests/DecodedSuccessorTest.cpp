#include "gtest/gtest.h"
#include "luxir/util/DecodedSuccessor.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace luxir;

namespace {

void checkAllTargets(const int32_t* values, int32_t length) {
  for (int32_t start = 0; start < length; start++) {
    std::vector<int32_t> targets = {-1, 0, 1, 2 * length, 2 * length + 1};
    targets.push_back(values[start] - 1);
    targets.push_back(values[start]);
    targets.push_back(values[start] + 1);
    targets.push_back(values[length - 1] - 1);
    targets.push_back(values[length - 1]);
    for (int32_t target : targets) {
      if ((start > 0 && values[start - 1] >= target)
          || values[length - 1] < target) {
        continue;
      }
      const int32_t expected = (int32_t) (
          std::lower_bound(values + start, values + length, target) - values);
      ASSERT_LT(expected, length);
      EXPECT_EQ(DecodedSuccessor::index(values, start, length, target),
                expected)
          << "length=" << length << " start=" << start
          << " target=" << target;
    }
  }
}

} // namespace

TEST(DecodedSuccessorTest, ExhaustiveLengthsStartsAndPartialVectors) {
  for (int32_t length = 1; length <= 128; length++) {
    std::vector<int32_t> values((size_t) length);
    for (int32_t i = 0; i < length; i++) {
      values[(size_t) i] = 2 * i + (i % 3 == 0 ? 0 : 1);
    }
    checkAllTargets(values.data(), length);
  }
}

TEST(DecodedSuccessorTest, DisableHookKeepsScalarSemantics) {
#if LUXIR_PROBE_CONSTANT_HOOKS
  const bool saved = DecodedSuccessor::disableSimdForTests;
  DecodedSuccessor::disableSimdForTests = true;
  const int32_t values[] = {2, 5, 9, 14, 20, 27, 35, 44, 54, 65,
                            77, 90, 104, 119, 135, 152, 170, 189};
  checkAllTargets(values, (int32_t) std::size(values));
  DecodedSuccessor::disableSimdForTests = saved;
#endif
}

TEST(DecodedSuccessorTest, TailLoadDoesNotCrossGuardPage) {
#if defined(__unix__)
  const long pageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(pageSize, 0);
  void* mapping = mmap(nullptr, (size_t) pageSize * 2,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mapping, MAP_FAILED);
  ASSERT_EQ(mprotect((char*) mapping + pageSize, (size_t) pageSize, PROT_NONE),
            0);

  for (int32_t length = 1; length <= 128; length++) {
    int32_t* values = (int32_t*) ((char*) mapping + pageSize
                                  - (int64_t) length * sizeof(int32_t));
    for (int32_t i = 0; i < length; i++) {
      values[i] = 3 * i + 1;
    }
    for (int32_t start = std::max(0, length - 20);
         start < length; start++) {
      for (int32_t target : {values[std::min(start, length - 1)],
                             values[length - 1]}) {
        const int32_t expected = (int32_t) (
            std::lower_bound(values + start, values + length, target)
            - values);
        ASSERT_EQ(DecodedSuccessor::index(values, start, length, target),
                  expected)
            << "length=" << length << " start=" << start;
      }
    }
  }
  ASSERT_EQ(munmap(mapping, (size_t) pageSize * 2), 0);
#else
  GTEST_SKIP() << "guard-page allocation requires mmap";
#endif
}

TEST(DecodedSuccessorTest, ExpectedIsaIsCompiled) {
#if defined(LUXIR_EXPECT_NO_AVX512)
  EXPECT_FALSE(DecodedSuccessor::hasAvx512ForTests());
#endif
#if defined(LUXIR_EXPECT_SCALAR)
  EXPECT_FALSE(DecodedSuccessor::hasAvx512ForTests());
  EXPECT_FALSE(DecodedSuccessor::hasAvx2ForTests());
#endif
}
