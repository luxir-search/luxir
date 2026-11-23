// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "bench/luxir_bench.h"
#include "luxir/LuxirConfig.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/index/CommitSnapshotRegistry.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/ReaderManager.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"

using namespace luxir;
using namespace luxir::test;

namespace {

void addSegments(CollectionHelper& helper, int segments, int docsPerSegment, int fields,
                 bool nested = false, int id = 0) {
  for (int seg = 0; seg < segments; ++seg) {
    CollectionHelper::UpdateBuilder update;
    for (int doc = 0; doc < docsPerSegment; ++doc, ++id) {
      auto number = std::to_string(id);
      auto text = "needle term" + number;
      std::string json = "{\"id\":\"" + number + '"';
      for (int field = 0; field < fields; ++field) {
        json += ",\"field" + std::to_string(field);
        if (field % 2 == 0) json += "_w\":\"" + text + '"';
        else json += "_i\":" + number;
      }
      if (nested) json += ",\"child_nested\":[{\"text_w\":\"" + text + "\",\"value_i\":" + number + "}]";
      json += '}';
      bool parsed = false;
      update.addRaw([&](auto& map, auto& arena) { parsed = api::read_json(map, json, arena); });
      ASSERT_TRUE(parsed);
    }
    auto result = helper.submit(update.commit());
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(0, result.total_errors);
  }
}

void BM_SegmentOpen(benchmark::State& state) {
  int segments = (int)state.range(0);
  int fields = (int)state.range(1);
  bool nested = state.range(2) != 0;
  bool fs = state.range(3) != 0;
  int docsPerSegment = unit_tests ? 2 : 32;

  LuxirConfig config;
  config.index.merge_factor = segments + 1;
  if (fs) {
    config.store.backend = "fs";
    config.store.data_dir = "/tmp/luxir-segment-open-XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(config.store.data_dir.data()));
  }
  auto cleanup = scope_guard([&] {
    if (fs) {
      std::error_code error;
      std::filesystem::remove_all(config.store.data_dir, error);
      EXPECT_FALSE(error) << error.message();
    }
  });
  LuxirNode node(config);
  CollectionHelper helper(node);
  ASSERT_NO_FATAL_FAILURE(addSegments(helper, segments, docsPerSegment, fields, nested));
  auto writer = helper.getIndexWriter();
  auto snapshot = *writer->snapshots.snapshot();
  ASSERT_EQ((size_t)segments, snapshot.openCores.size());
  snapshot.openCores.clear();

  for (auto _ : state) {
    BenchTimer timer(state);
    auto reader = std::make_shared<IndexReader>(writer->dir, snapshot);
    timer.stop();
    benchmark::DoNotOptimize(reader);
    if (unit_tests && nested) {
      EXPECT_EQ(segments * docsPerSegment, reader->view("child_nested")->liveDocs());
    }
  }
}

void BM_ReaderReopen(benchmark::State& state) {
  int segments = (int)state.range(0);
  int docsPerSegment = unit_tests ? 2 : 32;
  LuxirConfig config;
  config.index.merge_factor = segments + 2;
  LuxirNode node(config);
  CollectionHelper helper(node);
  ASSERT_NO_FATAL_FAILURE(addSegments(helper, segments, docsPerSegment, 8));
  auto writer = helper.getIndexWriter();
  auto previous = writer->snapshots.readers.getReader();
  ASSERT_EQ((size_t)segments, previous->segments().size());
  ASSERT_NO_FATAL_FAILURE(addSegments(helper, 1, 1, 8, false, segments * docsPerSegment));
  auto snapshot = writer->snapshots.snapshot();
  ASSERT_EQ((size_t)segments + 1, snapshot->openCores.size());
  ASSERT_TRUE(snapshot->openCores.back().expired());

  for (auto _ : state) {
    BenchTimer timer(state);
    auto reader = std::make_shared<IndexReader>(writer->dir, *snapshot, previous.get());
    timer.stop();
    benchmark::DoNotOptimize(reader);
    if (unit_tests) {
      ASSERT_EQ((size_t)segments + 1, reader->segments().size());
      for (int i = 0; i < segments; ++i) {
        EXPECT_EQ(previous->segments()[i].segmentCoreShared(), reader->segments()[i].segmentCoreShared());
      }
    }
  }
}

void BM_TermsEnumConstruct(benchmark::State& state) {
  int docs = unit_tests ? 256 : 131072;
  LuxirNode node;
  CollectionHelper helper(node);
  ASSERT_NO_FATAL_FAILURE(addSegments(helper, 1, docs, 1));
  auto writer = helper.getIndexWriter();
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& postings = reader->segments().front().postingsReader();
  auto* field = postings.fieldInfo("field0_w");
  ASSERT_NE(nullptr, field);

  MemPool pool;
  for (auto _ : state) {
    MemPool::ScopeGuard rewind(pool);
    BenchTimer timer(state);
    TermsEnum terms(pool, postings, *field);
    if (!terms.seek("needle")) {
      timer.stop();
      state.SkipWithError("Missing term 'needle'");
      FAIL() << "Missing term 'needle'";
    }
    auto postingsState = terms.postingsState();
    benchmark::DoNotOptimize(postingsState);
    timer.stop();
    if (unit_tests) {
      EXPECT_EQ(docs, postingsState.docFreq);
    }
  }
}

BENCHMARK(BM_SegmentOpen)
    ->ArgNames({"segments", "fields", "nested", "fs"})
    ->ArgsProduct({{1, 16, 64}, {8, 64}, {0, 1}, {0, 1}})
    ->UseManualTime();
BENCHMARK(BM_ReaderReopen)->ArgName("segments")->Arg(16)->Arg(64)->UseManualTime();
BENCHMARK(BM_TermsEnumConstruct)->UseManualTime();

} // namespace
