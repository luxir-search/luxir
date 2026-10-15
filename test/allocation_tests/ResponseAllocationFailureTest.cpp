// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "luxir/util/ReservedHandler.h"
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <new>

namespace {
// Wrap real allocations only in this executable. The main test/benchmark binary
// and production server retain their normal allocator and allocation cost.
struct AllocationFailure {
  static thread_local AllocationFailure* active;
  int remaining;
  void* first = nullptr;
  bool released = false;

  explicit AllocationFailure(int remaining) : remaining(remaining) { active = this; }
  ~AllocationFailure() { active = nullptr; }
};
thread_local AllocationFailure* AllocationFailure::active = nullptr;

void recordDelete(void* ptr) {
  auto* failure = AllocationFailure::active;
  if (failure && ptr && ptr == failure->first) failure->released = true;
}
} // namespace

extern "C" void* __real__Znwm(size_t size);
extern "C" void __real__ZdlPv(void* ptr);
extern "C" void __real__ZdlPvm(void* ptr, size_t size);

extern "C" void* __wrap__Znwm(size_t size) {
  auto* failure = AllocationFailure::active;
  if (failure && failure->remaining >= 0 && failure->remaining-- == 0) {
    throw std::bad_alloc();
  }
  void* ptr = __real__Znwm(size);
  if (failure && !failure->first) failure->first = ptr;
  return ptr;
}
extern "C" void __wrap__ZdlPv(void* ptr) {
  recordDelete(ptr);
  __real__ZdlPv(ptr);
}
extern "C" void __wrap__ZdlPvm(void* ptr, size_t size) {
  recordDelete(ptr);
  __real__ZdlPvm(ptr, size);
}

TEST(ResponseAllocationFailureTest, releasesFreshArenaOnEveryAllocationFailure) {
  luxir::LuxirNode node;
  luxir::test::CollectionHelper collection(node, "response_allocation_failure");
  auto request = luxir::test::localReq(node.getSearchEngine());
  request->reader = collection.collection().getReaderManager().getReader();

  bool succeeded = false;
  int constructionFailures = 0;
  for (int failAt = 0; failAt < 16 && !succeeded; ++failAt) {
    bool allocated;
    bool released;
    {
      AllocationFailure failure(failAt);
      try {
        auto* response = luxir::SearchResponse::create(*request, false);
        failure.remaining = -1;
        luxir::releaseArena(&response->arena);
        succeeded = true;
      } catch (const std::bad_alloc&) {
        if (failure.first) ++constructionFailures;
      }
      allocated = failure.first != nullptr;
      released = failure.released;
    }
    // The first successful allocation is createArena()'s combined backing block.
    EXPECT_TRUE(!allocated || released) << "failure at allocation " << failAt;
  }
  EXPECT_GT(constructionFailures, 0);
  EXPECT_TRUE(succeeded);

  auto* final = luxir::SearchResponse::create(*request);
  EXPECT_EQ(&request->arena, &final->arena); // The request still owns the final arena.
}

TEST(ResponseAllocationFailureTest, reservedCompletionCanPostWithHeapExhausted) {
  boost::asio::io_context context;
  boost::asio::any_io_executor executor = context.get_executor();
  luxir::ReservedHandlerAllocator<> allocator;
  int completed = 0;
  auto post = [&] {
    AllocationFailure failure(0);
    boost::asio::post(executor, boost::asio::bind_allocator(allocator, [&] { ++completed; }));
    EXPECT_EQ(0, failure.remaining); // No operator new call was needed.
  };
  ASSERT_NO_THROW(post());
  context.run();
  context.restart();
  ASSERT_NO_THROW(post()); // The first handler returned the reservation.
  context.run();
  EXPECT_EQ(2, completed);
}
