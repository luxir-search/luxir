// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/reader/Postings.h"
#include "test/TestIndex.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace luxir;
using namespace luxir::test;

TEST(UnpartitionedByteIdentityTest, DefaultMergeCorpus) {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS);
  TestIndex index;
  index.iw = std::make_unique<IndexWriter>(index.dir, schema);
  index.iw->mergePolicy->setMergeFactor(1000);
  // This test's contract is the UNPARTITIONED byte format; keep it serial
  // regardless of the debug-build tiny default thresholds.
  index.iw->termPartitionMinBytes = INT64_MAX;
  TestField body(index, "body");
  for (int32_t segment = 0; segment < 3; segment++) {
    body.startIndexing();
    for (int32_t doc = 0; doc < 12; doc++) {
      std::string value = "alpha beta common";
      value += " term" + std::to_string(doc % 5);
      value += " seg" + std::to_string(segment);
      body.add(doc, value);
    }
    index.flush();
  }
  index.iw->mergeSegments();
  index.initReader();
  ASSERT_EQ(1u, index.reader->segments().size());

  const char* dumpPath = std::getenv("LUXIR_BYTE_DUMP");
  if (dumpPath == nullptr) return;
  uint64_t segId = index.reader->segments()[0].segInfo.seg_id;
  std::string prefix = Postings::getIndexFileNamePrefix(segId);
  std::vector<Directory::FileInfo> infos;
  index.dir.listFiles(infos);
  std::vector<std::string> files;
  for (const auto& info : infos) files.push_back(info.name);
  std::sort(files.begin(), files.end());
  std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(dump.good());
  for (const auto& name : files) {
    if (!name.starts_with(prefix)) continue;
    auto input = index.dir.openFile(name);
    std::string_view bytes = input->read();
    uint64_t nameLen = name.size();
    uint64_t byteLen = bytes.size();
    dump.write((const char*) &nameLen, sizeof(nameLen));
    dump.write(name.data(), (std::streamsize) name.size());
    dump.write((const char*) &byteLen, sizeof(byteLen));
    dump.write(bytes.data(), (std::streamsize) bytes.size());
  }
  ASSERT_TRUE(dump.good());
}
