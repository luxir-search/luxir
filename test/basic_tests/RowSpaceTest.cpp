// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "test/RowSpaceFixture.h"
#include "test/CollectionHelper.h"
#include "test/QueryBuild.h"
#include "test/StorageTestUtils.h"
#include "luxir/index/LiveDocsWriter.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/search/DomainVariantPlan.h"
#include "luxir/search/OwnershipRelation.h"
#include "luxir/query/AllQuery.h"
#include "luxir/index/SegmentMerger.h"
#include "luxir/index/handler/FullTextHandler.h"
#include "luxir/index/handler/IntColHandler.h"
#include "luxir/index/handler/StrHandler.h"
#include "luxir/index/handler/VectorHandler.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/reader/ColumnShape.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/Uuid.h"

namespace luxir::test {
namespace {

using Fixture = RowSpaceFixture;
const std::vector<Fixture::Row> ROOTS{
    {101, {"root"}, "root", "root zero"},
    {102, {"root"}, "root", "root one"},
    {103, {"root"}, "root", "root two"}};
const std::vector<Fixture::Row> CHILDREN{
    {11, {"shared", "shared"}, "a", "child zero"},
    {},
    {11, {"other", "shared"}, "bb", "child two"},
    {22, {"other"}, "a", {}}};
const std::vector<int64_t> ENDS{2, 2, 4};
const std::vector<int64_t> OWNERS{0, 0, 2, 2};

class RowSpaceTest : public LuxirTest {
protected:
  RAMDir dir;

  std::shared_ptr<SegmentCore> open() {
    Fixture::write(dir, 1, ROOTS, CHILDREN, ENDS, OWNERS);
    return SegmentCore::create(dir, 1, 1);
  }
};

TEST_F(RowSpaceTest, persistedBundleUsesExistingReadersInBothSpaces) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-spaces-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirectory dir(path);
  auto files = Fixture::write(dir, 1, ROOTS, CHILDREN, ENDS, OWNERS);
  Fixture::write(dir, 2, ROOTS, {}, {});
  {
    SegmentCore core(dir, 1, 1);
    auto* child = core.spaceReader(Fixture::CHILD);
    ASSERT_NE(nullptr, child);
    EXPECT_EQ(&core.rootReader(), core.spaceReader(SpaceOrdinal::ROOT));
    EXPECT_EQ(3, core.rootReader().maxDoc());
    EXPECT_EQ(4, child->maxDoc());
    EXPECT_EQ(child, core.spaceReader(child->spaceOrdinal()));
    EXPECT_EQ(Fixture::CHILD, core.spaces()[(size_t)child->spaceOrdinal()].path);
    EXPECT_NE(core.rootReader().fieldInfo("number"), child->fieldInfo("items.number"));
    EXPECT_EQ(nullptr, core.spaceReader("missing"));
    EXPECT_EQ(nullptr, core.ownership(SpaceOrdinal::ROOT));
    VersionRuns::Cursor births(core.versionRuns());
    EXPECT_EQ(0u, births.birth(0));
  }
  // A second open resolves the same catalog without retaining the first core.
  SegmentCore core(dir, 1, 1);
  auto& child = *core.spaceReader(Fixture::CHILD);
  EXPECT_EQ(0u, child.fieldCacheStats().entries);
  EXPECT_EQ(2u, core.spaces().size());
  for (const auto& file : files) {
    auto input = dir.openFile(file.name);
    ASSERT_NE(nullptr, input);
    EXPECT_EQ(file.size, input->size());
  }
  SegmentCore absent(dir, 2, 1);
  EXPECT_EQ(1u, absent.spaces().size());
  EXPECT_EQ(nullptr, absent.spaceReader(Fixture::CHILD));
  EXPECT_EQ(nullptr, absent.ownership(Fixture::CHILD));
}

TEST_F(RowSpaceTest, childNumericColumnPreservesSparseRowsAndRepeatedValues) {
  auto core = open();
  auto& child = *core->spaceReader(Fixture::CHILD);
  auto* number = child.fieldInfo("items.number");
  ASSERT_NE(nullptr, number);
  EXPECT_EQ(3, number->docsWithField);
  IntColReader roots(core->rootReader(), *core->rootReader().fieldInfo("number"));
  IntColReader::SparseIterator rootValues(roots);
  EXPECT_EQ(0, rootValues.advance(0)); EXPECT_EQ(101, rootValues.value());
  IntColReader ints(child, *number);
  IntColReader::SparseIterator values(ints);
  EXPECT_EQ(0, values.advance(0)); EXPECT_EQ(11, values.value());
  EXPECT_EQ(2, values.advance(1)); EXPECT_EQ(11, values.value());
  EXPECT_EQ(3, values.advance(3)); EXPECT_EQ(22, values.value());
  EXPECT_EQ(PostingsReader::END, values.advance(4));
}

TEST_F(RowSpaceTest, childTermsAndPostingsUseTheirOwnFieldCatalog) {
  auto core = open();
  auto& child = *core->spaceReader(Fixture::CHILD);
  MemPool pool;
  TermsEnum terms(pool, child, *child.fieldInfo("items.body"));
  ASSERT_TRUE(terms.seek("shared"));
  DocsFreqEnum docs(terms);
  EXPECT_EQ(0, docs.nextDoc()); EXPECT_EQ(2, docs.termFreq());
  EXPECT_EQ(2, docs.nextDoc()); EXPECT_EQ(1, docs.termFreq());
  EXPECT_EQ(PostingsReader::END, docs.nextDoc());
  EXPECT_EQ(5, child.fieldInfo("items.body")->sumTotalTermFreq);
  TermsEnum rootTerms(pool, core->rootReader(), *core->rootReader().fieldInfo("body"));
  EXPECT_FALSE(rootTerms.seek("shared"));
}

TEST_F(RowSpaceTest, childStringColumnKeepsSpaceLocalDensityAndPresence) {
  auto core = open();
  auto& child = *core->spaceReader(Fixture::CHILD);
  auto* label = child.fieldInfo("items.label");
  MemPool pool;
  OrdColReader strings(child, *label);
  EXPECT_TRUE(strings.docIdIndexed());
  TermsEnum labels(pool, child, *label);
  labels.seekOrd(strings.ordAt(0) - 1); EXPECT_EQ("a", (std::string_view)labels.term());
  labels.seekOrd(strings.ordAt(2) - 1); EXPECT_EQ("bb", (std::string_view)labels.term());
  EXPECT_EQ(0, strings.ordAt(1));
  ColumnShape shape(child, *label);
  ColumnShape::Cursor cursor(shape);
  EXPECT_FALSE(cursor.seek(1));
  EXPECT_TRUE(cursor.seek(2)); EXPECT_EQ(1, cursor.begin()); EXPECT_EQ(2, cursor.end());
}

TEST_F(RowSpaceTest, childStoredFieldsIncludeEmptyObjectRows) {
  auto core = open();
  auto& child = *core->spaceReader(Fixture::CHILD);
  StoredFieldsReader stored(child, *child.fieldInfo(Postings::STORED_DEFAULT_RESOURCE));
  EXPECT_EQ(4, stored.maxDoc());
  std::vector<std::string> payloads;
  for (int32_t doc = 0; doc < child.maxDoc(); doc++) {
    stored.readDoc(doc, [&](std::string_view, std::span<const std::string_view> values) {
      for (auto value : values) payloads.emplace_back(value);
    });
  }
  EXPECT_EQ((std::vector<std::string>{"child zero", "child two"}), payloads);
}

TEST_F(RowSpaceTest, ownershipRangesAndReverseSeekPreserveEmptyOwners) {
  for (auto reverse : {std::optional<std::span<const int64_t>>{}, std::optional<std::span<const int64_t>>(OWNERS)}) {
    RAMDir dir;
    Fixture::write(dir, 1, ROOTS, CHILDREN, ENDS, reverse);
    SegmentCore core(dir, 1, 1);
    const auto* shape = core.ownership(Fixture::CHILD);
    ASSERT_NE(nullptr, shape);
    EXPECT_EQ(reverse.has_value(), shape->hasReverseMap());
    EXPECT_EQ(3, shape->parentCount()); EXPECT_EQ(4, shape->childCount());
    EXPECT_EQ((std::pair<int32_t, int32_t>{0, 2}), shape->range(0));
    EXPECT_EQ((std::pair<int32_t, int32_t>{2, 2}), shape->range(1));
    EXPECT_EQ((std::pair<int32_t, int32_t>{2, 4}), shape->range(2));
    OwnershipShape::Cursor cursor(*shape);
    for (int32_t child : {0, 1, 2, 3, 0, 3}) {
      EXPECT_EQ(OWNERS[child], cursor.owner(child).owner);
      EXPECT_EQ(OWNERS[child], shape->owner(child));
    }
  }
}

TEST_F(RowSpaceTest, openNeedsNoChildRowMapOrScan) {
  RAMDir dir;
  PostingsWriter writer(dir, 1, 1, 1);
  const int32_t children = 100000000;
  writer.finishSpace(writer.addSpace(Fixture::CHILD), children, Fixture::shape(writer, std::vector<int64_t>{children}));
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  EXPECT_LT(core.sizeInBytes(), 1024u);
  EXPECT_EQ(children, core.spaceReader(Fixture::CHILD)->maxDoc());
  EXPECT_FALSE(core.ownership(Fixture::CHILD)->hasReverseMap());
  EXPECT_EQ((std::pair<int32_t, int32_t>{0, children}), core.ownership(Fixture::CHILD)->range(0));
  EXPECT_EQ(0, core.ownership(Fixture::CHILD)->owner(children - 1));
}

TEST_F(RowSpaceTest, ownershipSeeksAcrossMonoBlockBoundaries) {
  const int32_t parents = (int32_t)MonoReader::BLOCK_SIZE + 3;
  std::vector<int64_t> ends, owners;
  for (int32_t parent = 0; parent < parents; parent++) {
    for (int32_t j = 0; j < parent % 3; j++) owners.push_back(parent);
    ends.push_back((int64_t)owners.size());
  }
  PostingsWriter writer(dir, 1, 1, parents);
  writer.finishSpace(writer.addSpace(Fixture::CHILD), (int32_t)owners.size(), Fixture::shape(writer, ends));
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  const auto& shape = *core.ownership(Fixture::CHILD);
  OwnershipShape::Cursor cursor(shape);
  for (int32_t child = 0; child < (int32_t)owners.size(); child += 7) EXPECT_EQ(owners[child], cursor.owner(child).owner);
  for (int32_t parent : {parents - 1, 0, parents - 2}) {
    EXPECT_EQ((std::pair<int32_t, int32_t>{parent ? (int32_t)ends[parent - 1] : 0, (int32_t)ends[parent]}), shape.range(parent));
  }
}

TEST_F(RowSpaceTest, ownershipBoundaryRunsMatchRangesAcrossFrames) {
  const int32_t parents = 2 * (int32_t)MonoReader::BLOCK_SIZE + 37;
  std::vector<int64_t> ends, deep;
  int64_t children = 0, parts = 0;
  for (int32_t parent = 0; parent < parents; parent++) {
    int32_t count = parent < 4 || parent >= parents - 4 ? 0
        : parent == 131 ? 3 * (int32_t)MonoReader::BLOCK_SIZE + 9 : parent % 5;
    for (int32_t i = 0; i < count; i++, children++) {
      parts += children % 7 == 0 ? 0 : children % 3;
      deep.push_back(parts);
    }
    ends.push_back(children);
  }
  PostingsWriter writer(dir, 1, 1, parents);
  writer.finishSpace(writer.addSpace(Fixture::CHILD), (int32_t)children, Fixture::shape(writer, ends));
  writer.finishSpace(writer.addSpace("items.deep"), (int32_t)parts, Fixture::shape(writer, deep));
  writeUnversionedRuns(writer); ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  const auto& shape = *core.ownership(Fixture::CHILD);
  OwnershipMapper mapper(core, "", "items.deep");
  for (auto [first, end] : std::vector<std::pair<int32_t, int32_t>>{
      {0, 0}, {0, 1}, {0, 128}, {0, 4096}, {127, 129}, {128, 256},
      {129, 4131}, {4095, parents}, {0, parents}, {parents - 1, parents}, {parents, parents}}) {
    SCOPED_TRACE(::testing::Message() << first << '/' << end);
    std::vector<int64_t> run((size_t)(end - first) + 1);
    memtrack::AllocScope allocation;
    shape.readBoundaries(first, end, run);
    auto bytes = allocation.bytes();
    if (memtrack::counting_enabled) { EXPECT_EQ(0, bytes); }
    for (int32_t owner = first; owner < end; owner++) {
      auto range = shape.range(owner);
      EXPECT_EQ(range.first, run[(size_t)(owner - first)]);
      EXPECT_EQ(range.second, run[(size_t)(owner - first) + 1]);
    }
    EXPECT_EQ(first ? ends[(size_t)first - 1] : 0, run.front());
    mapper.readBoundaries(first, end, run);
    for (int32_t owner = first; owner < end; owner++) {
      auto range = mapper.range(owner);
      EXPECT_EQ(range.first, run[(size_t)(owner - first)]);
      EXPECT_EQ(range.second, run[(size_t)(owner - first) + 1]);
    }
    int64_t boundary = first ? ends[(size_t)first - 1] : 0;
    EXPECT_EQ(boundary ? deep[(size_t)boundary - 1] : 0, run.front());
  }
}

TEST_F(RowSpaceTest, ownershipSeeksSkipEmptyFamiliesAndRestart) {
  const int32_t block = (int32_t)MonoReader::BLOCK_SIZE;
  std::vector<int64_t> ends, owners;
  for (int32_t parent = 0; parent < 18 * block + 3; parent++) {
    int32_t count = parent == block - 1 ? 3 * block + 17
        : parent >= 2 * block && parent < 12 * block ? 0 : parent % 11 == 0 ? 2 : 0;
    owners.insert(owners.end(), count, parent);
    ends.push_back((int64_t)owners.size());
  }
  std::vector<int32_t> children{0, (int32_t)ends[block - 2], (int32_t)ends[block - 2] + block,
      (int32_t)ends[block - 1] - 1, (int32_t)ends[block], (int32_t)ends[2 * block - 1],
      (int32_t)ends[14 * block], (int32_t)owners.size() - 1, 0,
      (int32_t)ends[16 * block], (int32_t)ends[block - 2] + 2 * block};
  for (int32_t i = 0; i < 64; i++) children.push_back(rng.rint(0, (int32_t)owners.size()));
  for (bool reverse : {false, true}) {
    RAMDir dir;
    PostingsWriter writer(dir, 1, 1, (int32_t)ends.size());
    auto optionalOwners = reverse ? std::optional<std::span<const int64_t>>(owners) : std::nullopt;
    writer.finishSpace(writer.addSpace(Fixture::CHILD), (int32_t)owners.size(), Fixture::shape(writer, ends, optionalOwners));
    writeUnversionedRuns(writer);
    ASSERT_TRUE(writer.finish());
    SegmentCore core(dir, 1, 1);
    memtrack::AllocScope construction;
    OwnershipShape::Cursor cursor(*core.ownership(Fixture::CHILD));
    auto constructionBytes = construction.bytes();
    if (memtrack::counting_enabled) { EXPECT_EQ(0, constructionBytes); }
    for (int32_t child : children) {
      memtrack::AllocScope allocations;
      auto family = cursor.owner(child);
      auto bytes = allocations.bytes();
      if (memtrack::counting_enabled) { EXPECT_EQ(0, bytes); }
      int32_t parent = (int32_t)(std::find_if(ends.begin(), ends.end(),
          [&](int64_t end) { return end > child; }) - ends.begin());
      EXPECT_EQ(parent, family.owner);
      EXPECT_EQ(parent ? ends[parent - 1] : 0, family.first);
      EXPECT_EQ(ends[parent], family.end);
    }
    for (int32_t parent : {0, 1, block - 1, block, 5 * block, 12 * block, 18 * block, 1, (int32_t)ends.size()}) {
      EXPECT_EQ(parent ? ends[parent - 1] : 0, cursor.firstChild(parent));
      if (parent < (int32_t)ends.size()) {
        auto family = cursor.seek(parent);
        EXPECT_EQ(parent, family.owner);
        EXPECT_EQ(parent ? ends[parent - 1] : 0, family.first);
        EXPECT_EQ(ends[parent], family.end);
        EXPECT_EQ(family.end, cursor.firstChild(parent + 1));
      }
    }
    EXPECT_EQ(0, cursor.owner(0).owner);
  }
}

TEST_F(RowSpaceTest, composedOwnershipCursorReturnsDescendantFamily) {
  const int32_t block = (int32_t)MonoReader::BLOCK_SIZE;
  std::vector<int64_t> ends{block + 1, block + 1, 3 * block + 7, 3 * block + 8};
  std::vector<int64_t> deep;
  int64_t rows = 0;
  for (int32_t parent = 0; parent < ends.back(); parent++) {
    rows += parent > block && parent < 2 * block ? 0 : parent % 3;
    deep.push_back(rows);
  }
  PostingsWriter writer(dir, 1, 1, (int32_t)ends.size());
  writer.finishSpace(writer.addSpace(Fixture::CHILD), (int32_t)ends.back(), Fixture::shape(writer, ends));
  writer.finishSpace(writer.addSpace("items.deep"), (int32_t)deep.back(), Fixture::shape(writer, deep));
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  OwnershipMapper mapper(core, "", "items.deep");
  MemPool pool;
  OwnershipMapper::Cursor cursor(mapper, pool);
  for (int32_t child : {0, 7, (int32_t)deep[block], (int32_t)deep.back() - 1, 0, (int32_t)deep[2 * block]}) {
    auto family = cursor.owner(child);
    int32_t parent = 0;
    while (deep[ends[parent] - 1] <= child) parent++;
    EXPECT_EQ(parent, family.owner);
    EXPECT_EQ(parent ? deep[ends[parent - 1] - 1] : 0, family.first);
    EXPECT_EQ(deep[ends[parent] - 1], family.end);
    EXPECT_EQ(family.first, cursor.firstChild(parent));
    EXPECT_EQ(family.end, cursor.firstChild(parent + 1));
  }
  for (int32_t parent : {0, 1, 2, 3, 0, 2}) {
    auto family = cursor.seek(parent);
    EXPECT_EQ(parent, family.owner);
    EXPECT_EQ(parent ? deep[ends[parent - 1] - 1] : 0, family.first);
    EXPECT_EQ(deep[ends[parent] - 1], family.end);
  }
}

TEST_F(RowSpaceTest, pathLookupBindsFieldsAndImmediateOwnership) {
  PostingsWriter writer(dir, 1, 1, 3);
  auto items = writer.addSpace("z");
  writer.finishSpace(items, 4, Fixture::shape(writer, ENDS));
  Fixture::fields(writer, items, "z", CHILDREN);
  writer.finishSpace(writer.addSpace("z.parts"), 2, Fixture::shape(writer, std::vector<int64_t>{1, 1, 2, 2}));
  auto earlier = writer.addSpace("a");
  writer.finishSpace(earlier, 3, Fixture::shape(writer, std::vector<int64_t>{1, 1, 3}));
  Fixture::fields(writer, earlier, "a", ROOTS);
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  std::vector<std::string> paths;
  for (const auto& space : core.spaces()) paths.push_back(space.path);
  EXPECT_EQ((std::vector<std::string>{"", "a", "z", "z.parts"}), paths);
  for (const auto& [path, expected] : std::vector<std::pair<std::string_view, int64_t>>{{"a", 101}, {"z", 11}}) {
    auto* reader = core.spaceReader(path);
    ASSERT_NE(nullptr, reader);
    EXPECT_EQ(reader, core.spaceReader(reader->spaceOrdinal()));
    IntColReader column(*reader, *reader->fieldInfo(std::string(path) + ".number"));
    IntColReader::SparseIterator values(column);
    EXPECT_EQ(0, values.advance(0)); EXPECT_EQ(expected, values.value());
  }
  EXPECT_EQ(4, core.ownership("z.parts")->parentCount());
  EXPECT_EQ(2, core.ownership("z.parts")->owner(1));
}

TEST_F(RowSpaceTest, invalidOwnershipEndpointsFailOpen) {
  auto write = [&](std::span<const int64_t> ends, std::optional<std::span<const int64_t>> owners) {
    dir.clear();
    PostingsWriter writer(dir, 1, 1, 3);
    writer.finishSpace(writer.addSpace(Fixture::CHILD), 4, Fixture::shape(writer, ends, owners));
    writeUnversionedRuns(writer);
    ASSERT_TRUE(writer.finish());
  };
  write(std::vector<int64_t>{2, 2, 5}, {});
  EXPECT_THROW(SegmentCore(dir, 1, 1), std::runtime_error);
  write(ENDS, std::span<const int64_t>(std::vector<int64_t>{0, 0, 2, 3}));
  EXPECT_THROW(SegmentCore(dir, 1, 1), std::runtime_error);
}

TEST_F(RowSpaceTest, ownershipStreamsStayInTheSharedFileInventory) {
  PostingsWriter writer(dir, 1, 1, 3);
  auto footer = writer.getOutputStream();
  auto shape = Fixture::shape(writer, ENDS, OWNERS);
  footer.reset();
  uint32_t pinned = shape.ends.filenum();
  writer.protectOutputFiles(std::span(&pinned, 1));
  writer.finishSpace(writer.addSpace(Fixture::CHILD), 4, shape);
  writeUnversionedRuns(writer);
  std::vector<FileDescriptor> files;
  ASSERT_TRUE(writer.finish(nullptr, &files));
  EXPECT_GT(files.size(), 1u);
  SegmentCore core(dir, 1, 1);
  EXPECT_EQ(pinned, core.spaces()[1].ownership->ends.filenum());
  EXPECT_EQ((std::pair<int32_t, int32_t>{2, 4}), core.ownership(Fixture::CHILD)->range(2));
  EXPECT_EQ(2, core.ownership(Fixture::CHILD)->owner(3));
  uint64_t bytes = 0;
  for (const auto& file : files) bytes += file.size;
  EXPECT_EQ(bytes, core.sizeInBytes());
}

TEST_F(RowSpaceTest, multiSpaceMergePreservesRowsAndShape) {
  RAMDir dir, output;
  {
    PostingsWriter source(dir, 1, 1, (int32_t)ROOTS.size());
    Fixture::fields(source, SpaceOrdinal::ROOT, "", ROOTS, std::vector<uint8_t>{1, 1, 1});
    auto child = source.addSpace(Fixture::CHILD);
    source.finishSpace(child, (int32_t)CHILDREN.size(), Fixture::shape(source, ENDS));
    Fixture::fields(source, child, Fixture::CHILD, CHILDREN, std::vector<uint8_t>{2, 2, 1});
    writeUnversionedRuns(source); ASSERT_TRUE(source.finish());
  }
  auto core = std::make_shared<SegmentCore>(dir, 1, 1);
  std::vector<std::shared_ptr<SegmentCore>> readers{core};
  std::vector<LiveDocs*> live{nullptr};
  PostingsWriter writer(output, 2, 1);
  IndexRamBudget budget;
  Schema schema;
  StoredDictionaryRegistry dictionaries;
  SegmentMerger merger(readers, live, writer, budget, INT64_MAX, 1, 4, schema, dictionaries);
  merger.merge();
  ASSERT_TRUE(writer.finish());
  SegmentCore merged(output, 2, 1);
  EXPECT_EQ(core->rootReader().maxDoc(), merged.rootReader().maxDoc());
  ASSERT_NE(nullptr, merged.spaceReader(Fixture::CHILD));
  EXPECT_EQ((int32_t)CHILDREN.size(), merged.spaceReader(Fixture::CHILD)->maxDoc());
  for (int32_t p = 0; p < (int32_t)ROOTS.size(); p++) {
    EXPECT_EQ(core->ownership(Fixture::CHILD)->range(p), merged.ownership(Fixture::CHILD)->range(p));
  }
}

TEST_F(RowSpaceTest, derivedLivenessClearsRangesAndWalksOwnerChains) {
  PostingsWriter writer(dir, 1, 1, 5);
  writer.finishSpace(writer.addSpace(Fixture::CHILD), 205, Fixture::shape(writer, std::vector<int64_t>{70, 70, 200, 200, 205}));
  std::vector<int64_t> ends(205, 2);
  std::fill(ends.begin() + 200, ends.end(), 3);
  writer.finishSpace(writer.addSpace("items.deep"), 3, Fixture::shape(writer, ends));
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish(nullptr));
  auto core = SegmentCore::create(dir, 1, 1);
  auto words = FixedBitSet::allocate(5, true);
  words[0] &= ~((1ULL << 1) | (1ULL << 2)); // Root 1 has no children.
  auto roots = std::make_shared<LiveDocs>(std::move(words), 5, 3);
  auto base = std::make_shared<SegmentBase>(1, 1, 5, SegmentBase::SpaceRows{{"items", 205}, {"items.deep", 3}},
                                            core);
  auto rootLive = std::make_shared<SegmentLive>(1, 1, 1, 1, roots);
  ChildLiveDocs child(base, rootLive, "items");
  auto live = child.get();
  ASSERT_NE(nullptr, live);
  EXPECT_EQ(75, live->numLive());
  for (int32_t row = 0; row < 205; row++) EXPECT_EQ(row < 70 || row >= 200, live->docset().get(row));
  ChildLiveDocs deep(base, rootLive, "items.deep");
  EXPECT_EQ(nullptr, deep.get());
}


TEST_F(RowSpaceTest, spaceReservationRequiresFinalCountsBeforeFinish) {
  PostingsWriter writer(dir, 1, 1, 2);
  auto items = writer.addSpace("items");
  auto deep = writer.addSpace("items.deep");
  EXPECT_THROW(writer.addSpace("items"), std::invalid_argument);
  EXPECT_THROW(writer.addSpace("missing.deep"), std::invalid_argument);
  EXPECT_THROW(writer.getMaxDoc(items), std::invalid_argument);
  EXPECT_THROW(writer.finishSpace(items, 0, {}), std::invalid_argument);
  writeUnversionedRuns(writer);
  EXPECT_THROW(writer.finish(), std::invalid_argument);
  writer.finishSpace(deep, 1, Fixture::shape(writer, std::vector<int64_t>{0, 1}));
  writer.finishSpace(items, 2, Fixture::shape(writer, std::vector<int64_t>{1, 2}));
  EXPECT_THROW(writer.finishSpace(items, 2, {}), std::invalid_argument);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  EXPECT_EQ(2, core.spaceReader("items")->maxDoc());
  EXPECT_EQ(1, core.ownership("items.deep")->owner(0));
}

TEST_F(RowSpaceTest, physicalHandlersFlushAgainstTheirBoundSpace) {
  Inverter inverter(dir, 1, 1);
  auto& writer = inverter.getPostingsWriter();
  auto space = writer.addSpace("items_nested");
  auto bind = [&]<typename T>(std::string_view local) -> T& {
    auto name = Schema::qualify("items_nested", local);
    auto field = inverter.pool.make_unique<T>(inverter, name, inverter.schema->getFieldTypeEx(name), space);
    auto* result = field.get();
    inverter.indexHandlers.emplace(std::move(name), std::move(field));
    return *result;
  };
  auto& ints = bind.operator()<handler::IntColHandler>("number_i");
  auto& many = bind.operator()<handler::MultiIntColHandler>("many_is");
  auto& text = bind.operator()<handler::FullTextHandler>("body_w");
  auto& labels = bind.operator()<handler::StrHandler<>>("label_s");
  auto& raw = bind.operator()<handler::StrColHandler>("raw_sc");
  auto& vectors = bind.operator()<handler::VectorHandler>("embedding_v");
  for (int32_t row = 0; row < 3; row++) {
    inverter.startDoc();
    if (row != 1) ints.index(inverter, (int64_t)row + 10);
    const int64_t values[]{row, row + 10};
    many.index(inverter, std::span<const int64_t>(values));
    text.index(inverter, std::string_view("common common"));
    labels.index(inverter, std::string_view("label"));
    raw.index(inverter, std::string_view("raw"));
    const float value[]{(float)row, 1.0f};
    api::Val vector;
    vector.kind.emplace<api::Vector>().f32.emplace().v = value;
    vectors.index(inverter, vector);
  }
  inverter.setDoc(5); // The root and child counts deliberately differ.
  writer.finishSpace(space, 3, Fixture::shape(writer, std::vector<int64_t>{1, 1, 2, 2, 3, 3}));
  ASSERT_TRUE(inverter.flush());
  SegmentCore core(dir, 1, 1);
  auto& child = *core.spaceReader("items_nested");
  EXPECT_EQ(6, core.rootReader().maxDoc());
  EXPECT_EQ(3, child.maxDoc());
  for (auto name : {"number_i", "many_is", "body_w", "label_s", "raw_sc", "embedding_v"}) {
    auto qualified = Schema::qualify("items_nested", name);
    EXPECT_EQ(nullptr, core.rootReader().fieldInfo(qualified));
    ASSERT_NE(nullptr, child.fieldInfo(qualified));
  }
  IntColReader column(child, *child.fieldInfo("items_nested.number_i"));
  IntColReader::SparseIterator values(column);
  EXPECT_EQ(0, values.advance(0)); EXPECT_EQ(10, values.value());
  EXPECT_EQ(2, values.advance(1)); EXPECT_EQ(12, values.value());
  EXPECT_EQ(PostingsReader::END, values.advance(3));
  auto& body = *child.fieldInfo("items_nested.body_w");
  EXPECT_EQ(3, body.docsWithField);
  NormsReader norms(child, body);
  EXPECT_TRUE(norms.isFlat());
  EXPECT_EQ(3, norms.numBytes());
  EXPECT_EQ(2, norms.value(2));
  OrdColReader ords(child, *child.fieldInfo("items_nested.label_s"));
  EXPECT_TRUE(ords.docIdIndexed());
  EXPECT_NE(0, ords.ordAt(2));
  VectorReader vector(child, *child.fieldInfo("items_nested.embedding_v"));
  EXPECT_EQ(2, vector.dims());
}

TEST_F(RowSpaceTest, inverterStoredResourcesAreLocalToTheirBoundSpace) {
  Inverter inverter(dir, 1, 1);
  auto& writer = inverter.getPostingsWriter();
  auto space = writer.addSpace("items");
  auto& roots = inverter.getOrCreateStoredFields(SpaceOrdinal::ROOT, Postings::STORED_DEFAULT_RESOURCE);
  auto& children = inverter.getOrCreateStoredFields(space, Postings::STORED_DEFAULT_RESOURCE);
  EXPECT_NE(&roots, &children);
  EXPECT_EQ(&roots, &inverter.getOrCreateStoredFields(SpaceOrdinal::ROOT, Postings::STORED_DEFAULT_RESOURCE));
  roots.addValue(0, "payload", "root");
  children.addValue(0, "items.payload", "child");
  inverter.setDoc(5);
  writer.finishSpace(space, 3, Fixture::shape(writer, std::vector<int64_t>{1, 1, 2, 2, 3, 3}));
  ASSERT_TRUE(inverter.flush());
  SegmentCore core(dir, 1, 1);
  for (auto path : {"", "items"}) {
    auto& reader = *core.spaceReader(path);
    StoredFieldsReader stored(reader, *reader.fieldInfo(Postings::STORED_DEFAULT_RESOURCE));
    EXPECT_EQ(path == std::string_view("") ? 6 : 3, stored.maxDoc());
    stored.readDoc(0, [&](std::string_view name, std::span<const std::string_view> values) {
      EXPECT_EQ(path == std::string_view("") ? "payload" : "items.payload", name);
      ASSERT_EQ(1u, values.size());
      EXPECT_EQ(path == std::string_view("") ? "root" : "child", values[0]);
    });
  }
}

class ScopedRowSpaceTest : public LuxirTest {
protected:
  std::filesystem::path directoryPath = std::filesystem::temp_directory_path() / ("luxir-views-" + newUuid());
  FSDirectory directory{directoryPath};
  const std::vector<Fixture::Row> roots{ROOTS[0], ROOTS[1], ROOTS[2], {}};
  const std::vector<int64_t> ends{2, 2, 4, 4};
  std::shared_ptr<FilterCache> cache = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 1});
  std::shared_ptr<const CommitSnapshot> snapshot;
  std::shared_ptr<IndexReader> root;

  void SetUp() override {
    auto schema = std::make_shared<Schema>();
    schema->fieldTypeMap["_version_"] = std::make_shared<IntFieldType>("_version_", 0);
    schema->fieldTypeMap["number"] = std::make_shared<IntFieldType>("number", FieldType::COLUMN_STORED);
    schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>("body", FieldType::INDEX_DOCS_FREQS, "whitespace");
    schema->fieldTypeMap["label"] = std::make_shared<StrFieldType>("label", FieldType::INDEX_DOCS | FieldType::COLUMN_STORED);
    schema->fieldTypeMap["embedding"] = std::make_shared<VectorFieldType>("embedding", 1,
        FieldType::COLUMN_STORED | FieldType::FIXED_SIZE, VectorFieldType::METRIC_L2);
    schema->fieldTypeMap["payload"] = std::make_shared<TextFieldType>("payload", FieldType::STORED, "whitespace");
    for (auto name : {"number", "body", "label", "embedding", "payload"}) {
      schema->fieldTypeMap.emplace(Schema::qualify(Fixture::CHILD, name), schema->fieldTypeMap.at(name));
    }
    for (auto name : {"items", "items.deep", "absent"}) {
      schema->fieldTypeMap.emplace(name, std::make_shared<NestedFieldType>(name));
    }
    schema->fieldTypeMap.emplace("absent.number", schema->fieldTypeMap.at("number"));
    CommitSnapshotRegistry snapshots(reserveIncarnation(directory));
    {
      IndexWriter writer(snapshots, schema);
      writer.commit();
    }
    snapshot = snapshots.readers.snapshot();
    publish();
  }

  void TearDown() override {
    root.reset();
    snapshot.reset();
    std::filesystem::remove_all(directoryPath);
  }

  void publish(int deletedBundle = -1) {
    std::pmr::monotonic_buffer_resource arena;
    auto manifest = Manifest::decode(snapshot->bytes, arena);
    auto previous = manifest.segments;
    auto* segments = build::allocArray(manifest.segments, 3, arena);
    for (int32_t i = 0; i < 3; i++) {
      auto& segment = segments[i];
      segment.seg_id = (uint64_t)i + 1;
      segment.creator_epoch = 1;
      segment.schema = *manifest.schema;
      segment.max_doc = segment.live_docs = (int32_t)roots.size();
      segment.min_version = 100 + (uint64_t)i * 10;
      segment.max_version = segment.min_version + roots.size() - 1;
      std::vector<FileDescriptor> files;
      if (previous.empty()) {
        PostingsWriter writer(directory, segment.seg_id, 1, (int32_t)roots.size());
        Fixture::fields(writer, SpaceOrdinal::ROOT, "", roots, std::vector<uint8_t>{1, 1, 1});
        if (i < 2) {
          auto child = writer.addSpace(Fixture::CHILD);
          writer.finishSpace(child, (int32_t)CHILDREN.size(), Fixture::shape(writer, ends));
          Fixture::fields(writer, child, Fixture::CHILD, CHILDREN, std::vector<uint8_t>{2, 2, 1});
          auto& vectors = writer.addField(child, "items.embedding");
          vectors.type = FieldType::VECTOR;
          vectors.flags = FieldType::COLUMN_STORED | FieldType::FIXED_SIZE;
          vectors.numValues = 4;
          vectors.mono2MetaOff = sizeof(float);
          {
            auto output = writer.getOutputStream();
            vectors.columnLoc = output->slocation();
            const float values[]{0, 1, 10, 11};
            output->write(values, sizeof(values));
            vectors.columnMetaOff = sizeof(values);
          }
          Fixture::presence(writer, vectors, std::vector<int32_t>{0, 1, 2, 3}, 4);
          writer.finishSpace(writer.addSpace("items.deep"), 4, Fixture::shape(writer, std::vector<int64_t>{1, 2, 3, 4}));
        }
        VersionRunBuilder versions;
        for (uint32_t row = 0; row < roots.size(); row++) versions.add(row, 100 + (uint64_t)i * 10 + row);
        writer.writeVersionRuns(versions);
        ASSERT_TRUE(writer.finish(nullptr, &files));
        if (i < 2) {
          auto* spaces = build::allocArray(segment.spaces, 2, arena);
          spaces[0] = {build::arenaStr(arena, Fixture::CHILD), (int32_t)CHILDREN.size()};
          spaces[1] = {build::arenaStr(arena, "items.deep"), 4};
        }
      } else {
        auto& old = previous[(size_t)i];
        segment.spaces = old.spaces;
        for (const auto& file : old.files) {
          files.push_back({std::string(file.name), file.size, file.xxh3});
        }
        segment.live_gen = old.live_gen;
        segment.live_epoch = old.live_epoch;
        segment.live_docs = old.live_docs;
      }
      if (i == deletedBundle && segment.live_gen == 0) {
        uint64_t word = 14;
        segment.live_gen = segment.live_epoch = 1;
        segment.live_docs = 3;
        LiveDocsWriter::writeLiveDocs(directory, segment.seg_id, 1, 1, 1, FixedBitSet(&word, 4), 4, 3, &files);
      }
      auto* descriptors = build::allocArray(segment.files, files.size(), arena);
      for (size_t f = 0; f < files.size(); f++) {
        descriptors[f] = {build::arenaStr(arena, files[f].name), files[f].size, files[f].xxh3};
      }
    }
    manifest.index_gen++;
    if (previous.empty()) manifest.core_gen++;
    manifest.commit_time++;
    auto bytes = std::make_shared<std::vector<std::byte>>();
    ASSERT_TRUE(api::encode(manifest, *bytes));
    snapshot = CommitSnapshot::fromBytes(bytes, snapshot->schema);
    root = std::make_shared<IndexReader>(directory, *snapshot, root.get(), cache);
    cache->onReaderPublished(*root);
  }

  LocalReqHandle request() { return localReq(luxirNode->getSearchEngine()); }
};

TEST_F(ScopedRowSpaceTest, viewsAlignLeavesAndKeepFieldStateSeparate) {
  auto child = root->view(Fixture::CHILD);
  EXPECT_EQ(child, root->view(Fixture::CHILD));
  EXPECT_EQ(root, child->view(""));
  EXPECT_EQ(12, root->maxDoc());
  EXPECT_EQ(8, child->maxDoc());
  EXPECT_EQ(8, child->liveDocs());
  ASSERT_EQ(3u, child->segments().size());
  EXPECT_EQ(0, child->segments()[2].maxDoc());
  EXPECT_FALSE(child->segments()[2].hasSpace());
  EXPECT_EQ(8, child->segments()[2].base);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(&root->segments()[i].segmentCore(), &child->segments()[i].segmentCore());
    EXPECT_EQ(4, child->segments()[i].segInfo.max_doc);
  }
  EXPECT_EQ(8, child->field("items.body").stats().maxDoc);
  EXPECT_EQ(6, child->field("items.body").stats().docsWithField);
  EXPECT_EQ(10, child->field("items.body").stats().sumTotalTermFreq);
  EXPECT_EQ(9, root->field("body").stats().sumTotalTermFreq);
  EXPECT_NE(child->getOrdMap("items.label"), root->getOrdMap("label"));
  MemPool pool;
  Query::Context context(pool, *child);
  Similarity::TermStats stats;
  ASSERT_TRUE(context.lookupTermStats("items.body", "shared", stats));
  EXPECT_EQ(4, stats.docFreq);
  EXPECT_EQ(6, stats.totalTermFreq);
  EXPECT_EQ(0, root->view("absent")->maxDoc());
  auto empty = request();
  empty->topDocs().allQuery().countLimit().fields({"number"});
  empty->execute(root->view("absent"));
  ASSERT_OK(empty);
  EXPECT_EQ(0, empty->getMatchCount());
}

TEST_F(ScopedRowSpaceTest, ordinaryQueriesSortFacetAndEmitChildRows) {
  auto child = root->view(Fixture::CHILD);
  auto term = request();
  term->topDocs().matchQuery("body", "shared").withStats().fields({"number", "payload"});
  term->execute(child);
  ASSERT_OK(term);
  EXPECT_EQ(4, term->getMatchCount());
  ASSERT_EQ(4u, term->getDocs().size());
  EXPECT_TRUE(docEquals(flatdoc("number", (int64_t)11, "payload", "child zero"), term->getDocs()[0]));
  auto scores = std::get<api::ColFloat>(term->docList()->columns.find("_score_")->kind).v;
  EXPECT_GT(scores[0], scores.back());

  auto numeric = request();
  auto& op = numeric->topDocs().existsQuery("number").countLimit().fields({"number"});
  qb::sort(op, "number", qb::DESC);
  numeric->execute(child, false);
  ASSERT_OK(numeric);
  EXPECT_EQ(6, numeric->getMatchCount());
  EXPECT_TRUE(docEquals(flatdoc("number", (int64_t)22), numeric->getDocs()[0]));

  auto all = request();
  all->topDocs().allQuery().countLimit().limit(0);
  all->facet("labels", "label").mincount(1);
  all->sum("total", "number");
  all->execute(child);
  ASSERT_OK(all);
  EXPECT_EQ(8, all->getMatchCount());
  EXPECT_EQ(88, all->scalar<int64_t>("total"));
  auto* facet = (*all->responses[0]->proto.ops.find("labels"))->facetResult();
  ASSERT_NE(nullptr, facet);
  EXPECT_EQ((std::vector<int64_t>{4, 2}), std::vector<int64_t>(facet->counts.begin(), facet->counts.end()));

  auto negated = request();
  auto& query = negated->topDocs().countLimit().limit(0);
  query.rawQuery() = qb::boolean(query.mr(), {qb::all()}, {}, {qb::match(query.mr(), "body", "shared")});
  negated->execute(child);
  ASSERT_OK(negated);
  EXPECT_EQ(4, negated->getMatchCount());
}

TEST_F(ScopedRowSpaceTest, cacheWarmOrderIncludesAllRowDomains) {
  auto child = root->view(Fixture::CHILD);
  for (bool childFirst : {false, true}) {
    cache->clear();
    cache->resetCounters();
    cache->resetAdmission();
    for (int32_t repeat = 0; repeat < 3; repeat++) {
      for (bool children : {childFirst, !childFirst}) {
        auto req = request();
        req->topDocs().allQuery().matchFilter("label", "a").countLimit().limit(0)
            .facet("labels", "label").mincount(1);
        req->topDocs("all").allQuery().countLimit().limit(0).filter(qb::all());
        req->execute(children ? child : root, false);
        ASSERT_OK(req);
        EXPECT_EQ(children ? 4 : 0, req->getMatchCount());
        EXPECT_EQ(children ? 8 : 12, req->getMatchCount("all"));
      }
    }
    EXPECT_GT(cache->counters().hits, 0u);
    cache->validateForTest();
  }
}

TEST_F(ScopedRowSpaceTest, readerStableCacheValuesBelongToTheirOutputView) {
  auto child = root->view(Fixture::CHILD);
  auto key = [](IndexReader& reader) {
    FilterKeyBuilder builder;
    builder.appendString("selection");
    return std::move(builder).finishStructural(FilterKeyScope::READER_STABLE, {.timeZone = {}, .path = reader.path()});
  };
  for (bool childFirst : {false, true}) {
    cache->clear();
    for (int32_t repeat = 0; repeat < 2; repeat++) {
      for (bool children : {childFirst, !childFirst}) {
        auto& view = children ? *child : *root;
        FilterCache::UseRegistry uses(*cache, view);
        auto* use = uses.get(key(view), FilterKeyScope::READER_STABLE);
        auto probe = use->probeReaderStable(view, {});
        if (repeat == 0) {
          ASSERT_EQ(FilterCache::ReaderProbe::Kind::BUILD, probe.kind());
          std::vector<std::unique_ptr<DocSet>> sets;
          for (auto& leaf : view.segments()) {
            DocSetBuilder builder(leaf.maxDoc());
            builder.addRange(0, leaf.maxDoc());
            sets.push_back(builder.build());
          }
          use->publishReaderStable(probe, std::move(sets), 1);
        } else ASSERT_EQ(FilterCache::ReaderProbe::Kind::HIT, probe.kind());
        for (auto& leaf : view.segments()) {
          auto domain = use->effectiveDomain((size_t)leaf.ord, view, nullptr);
          EXPECT_EQ(leaf.maxDoc(), domain.get(leaf)->card());
        }
      }
    }
    cache->validateForTest();
  }
}

#ifndef NDEBUG
TEST_F(ScopedRowSpaceTest, allRowDomainsKeepScopeThroughComposition) {
  auto child = root->view(Fixture::CHILD);
  auto all = DomainHandle().scoped(child->segments()[0]);
  auto composed = DomainVariantPlan::compose({all, all});
  EXPECT_EQ(nullptr, composed.get(child->segments()[0]));
  EXPECT_DEATH(composed.get(root->segments()[0]), "leaf == &segment");
  EXPECT_DEATH(DomainVariantPlan::compose({all, DomainHandle().scoped(root->segments()[0])}),
      "leaf == other.leaf");
}
#endif

TEST_F(ScopedRowSpaceTest, heldViewSurvivesNewSnapshotWithoutCycles) {
  auto child = root->view(Fixture::CHILD);
  auto oldRoot = root;
  std::pmr::monotonic_buffer_resource arena;
  auto manifest = Manifest::decode(snapshot->bytes, arena);
  manifest.segments = manifest.segments.subspan(1);
  manifest.index_gen++;
  manifest.core_gen++;
  manifest.commit_time++;
  auto bytes = std::make_shared<std::vector<std::byte>>();
  ASSERT_TRUE(api::encode(manifest, *bytes));
  auto newer = CommitSnapshot::fromBytes(bytes, snapshot->schema);
  root = std::make_shared<IndexReader>(directory, *newer, root.get(), cache);
  cache->onReaderPublished(*root);
  EXPECT_EQ(4, root->view(Fixture::CHILD)->maxDoc());
  EXPECT_EQ(8, child->maxDoc());
  std::weak_ptr<IndexReader> held = child;
  {
    auto req = request();
    req->topDocs().allQuery().countLimit().fields({"label"});
    req->execute(child);
    ASSERT_OK(req);
    EXPECT_EQ(8, req->getMatchCount());
  }
  child.reset();
  oldRoot.reset();
  EXPECT_TRUE(held.expired());
}

TEST_F(ScopedRowSpaceTest, openingViewsUsesCountsWithoutScanningChildRows) {
  const int32_t count = 100000000;
  PostingsWriter writer(directory, 4, 1, 1);
  writer.finishSpace(writer.addSpace(Fixture::CHILD), count, Fixture::shape(writer, std::vector<int64_t>{count}));
  writeUnversionedRuns(writer);
  std::vector<FileDescriptor> files;
  ASSERT_TRUE(writer.finish(nullptr, &files));
  std::pmr::monotonic_buffer_resource arena;
  auto manifest = Manifest::decode(snapshot->bytes, arena);
  auto previous = manifest.segments;
  auto* segments = build::allocArray(manifest.segments, 4, arena);
  std::copy(previous.begin(), previous.end(), segments);
  auto& segment = segments[3];
  segment.seg_id = 4;
  segment.creator_epoch = 1;
  segment.schema = *manifest.schema;
  segment.max_doc = segment.live_docs = 1;
  auto* spaces = build::allocArray(segment.spaces, 1, arena);
  spaces[0] = {build::arenaStr(arena, Fixture::CHILD), count};
  auto* descriptors = build::allocArray(segment.files, files.size(), arena);
  for (size_t i = 0; i < files.size(); i++) {
    descriptors[i] = {build::arenaStr(arena, files[i].name), files[i].size, files[i].xxh3};
  }
  manifest.index_gen++;
  manifest.core_gen++;
  manifest.commit_time++;
  auto bytes = std::make_shared<std::vector<std::byte>>();
  ASSERT_TRUE(api::encode(manifest, *bytes));
  auto next = CommitSnapshot::fromBytes(bytes, snapshot->schema);
  auto reopened = std::make_shared<IndexReader>(directory, *next, root.get(), cache);
  auto child = reopened->view(Fixture::CHILD);
  EXPECT_EQ(count + 8, child->maxDoc());
  EXPECT_EQ(count + 8, child->field("items.body").stats().maxDoc);
  EXPECT_LT(child->segments()[3].segmentCore().sizeInBytes(), 1024u);
  EXPECT_EQ(8, root->view(Fixture::CHILD)->maxDoc());
}

TEST_F(ScopedRowSpaceTest, childLivenessFollowsRootDeletesAndRetainsOldViews) {
  auto old = root->view(Fixture::CHILD);
  publish(2); // Deletes in an absent path's bundle do not affect the child view.
  EXPECT_EQ(8, root->view(Fixture::CHILD)->liveDocs());
  publish(0);
  auto child = root->view(Fixture::CHILD);
  EXPECT_EQ(6, child->liveDocs());
  EXPECT_EQ(8, old->liveDocs());
  EXPECT_FALSE(child->segments()[0].liveDocSet()->get(0));
  EXPECT_FALSE(child->segments()[0].liveDocSet()->get(1));
  EXPECT_TRUE(child->segments()[0].liveDocSet()->get(2));
  auto held = child->segments()[0].liveDocsShared();
  publish(1);
  EXPECT_EQ(held, root->view(Fixture::CHILD)->segments()[0].liveDocsShared());
  EXPECT_EQ(4, root->view(Fixture::CHILD)->liveDocs());
  auto req = request();
  req->topDocs().allQuery().countLimit().limit(0);
  req->topDocs("not_shared").countLimit().limit(0).rawQuery() = qb::boolean(req->mr,
      {qb::all()}, {}, {qb::match(req->mr, "body", "shared")});
  req->execute(child);
  ASSERT_OK(req);
  EXPECT_EQ(6, req->getMatchCount());
  EXPECT_EQ(3, req->getMatchCount("not_shared"));
}

TEST_F(ScopedRowSpaceTest, childBirthVersionsResolveTheirRootRows) {
  auto req = request();
  req->topDocs().allQuery().countLimit().fields({"_version_"});
  req->execute(root->view(Fixture::CHILD));
  ASSERT_OK(req);
  auto versions = std::get<api::ColInt>(req->docList()->columns.find("_version_")->kind).v;
  EXPECT_EQ((std::vector<int64_t>{100, 100, 102, 102, 110, 110, 112, 112}),
      std::vector<int64_t>(versions.begin(), versions.end()));
}

TEST_F(ScopedRowSpaceTest, joinReducersComposeWithRootQueries) {
  for (auto score : {"none", "max"}) {
    auto req = request();
    auto join = qb::toParent(req->mr, Fixture::CHILD, qb::match(req->mr, "body", "shared"), score);
    req->topDocs().withStats().fields({"number"}).rawQuery() = join;
    req->topDocs("and").countLimit().limit(0).rawQuery() = qb::boolean(req->mr,
        {join, qb::match(req->mr, "number", "101")});
    req->topDocs("not").countLimit().limit(0).rawQuery() = qb::boolean(req->mr, {qb::all()}, {}, {join});
    req->topDocs("filtered").countLimit().matchFilter("number", "103").rawQuery() = join;
    req->execute(root);
    ASSERT_OK(req);
    EXPECT_EQ(4, req->getMatchCount());
    EXPECT_EQ(2, req->getMatchCount("and"));
    EXPECT_EQ(8, req->getMatchCount("not"));
    EXPECT_EQ(2, req->getMatchCount("filtered"));
    if (score == std::string_view("none")) {
      auto scores = std::get<api::ColFloat>(req->docList()->columns.find("_score_")->kind).v;
      for (float value : scores) EXPECT_FLOAT_EQ(1, value);
    }
  }
}

TEST_F(ScopedRowSpaceTest, maximumJoinScoreIsTheChildMaximum) {
  auto children = request();
  children->topDocs().matchQuery("body", "shared").withStats().fields({"payload"});
  children->execute(root->view(Fixture::CHILD));
  ASSERT_OK(children);
  auto childScores = std::get<api::ColFloat>(children->docList()->columns.find("_score_")->kind).v;
  auto parents = request();
  parents->topDocs().withStats().fields({"number"}).rawQuery() = qb::toParent(parents->mr,
      Fixture::CHILD, qb::match(parents->mr, "body", "shared"));
  parents->execute(root);
  ASSERT_OK(parents);
  auto scores = std::get<api::ColFloat>(parents->docList()->columns.find("_score_")->kind).v;
  ASSERT_EQ(childScores.size(), scores.size());
  for (size_t i = 0; i < scores.size(); i++) EXPECT_FLOAT_EQ(childScores[i], scores[i]);
}

TEST_F(ScopedRowSpaceTest, maximumReducerVisitsAllMatchingSiblings) {
  auto req = request();
  req->topDocs().withStats().fields({"number"}).rawQuery() = qb::toParent(req->mr, Fixture::CHILD,
      qb::rescore(req->mr, qb::exists(req->mr, "number"), "number"));
  req->execute(root);
  ASSERT_OK(req);
  auto scores = std::get<api::ColFloat>(req->docList()->columns.find("_score_")->kind).v;
  ASSERT_EQ(4u, scores.size());
  EXPECT_EQ((std::vector<float>{22, 22, 11, 11}), std::vector<float>(scores.begin(), scores.end()));
}

TEST_F(ScopedRowSpaceTest, consumingChildSelectsWithinOwnerEligibility) {
  auto req = request();
  auto& list = req->topDocs().withStats().matchFilter("number", "103").fields({"number"});
  list.rawQuery() = qb::toParent(req->mr, Fixture::CHILD, qb::knn(req->mr, "embedding", {0}, 2, 0, true));
  req->execute(root);
  ASSERT_OK(req);
  EXPECT_EQ(2, req->getMatchCount());
  for (const auto& doc : req->getDocs()) EXPECT_TRUE(docEquals(flatdoc("number", (int64_t)103), doc));
}

TEST_F(ScopedRowSpaceTest, consumingChildUsesDerivedLiveEligibilityAfterDelete) {
  for (bool deleted : {false, true}) {
    if (deleted) publish(0);
    auto req = request();
    auto join = qb::toParent(req->mr, Fixture::CHILD, qb::knn(req->mr, "embedding", {0}, 2, 0, true));
    req->topDocs().allQuery().countLimit().limit(0).filter(join);
    req->execute(root);
    ASSERT_OK(req);
    EXPECT_EQ(deleted ? 1 : 2, req->getMatchCount());
  }
}

TEST_F(ScopedRowSpaceTest, streamingJoinDoesNotBuildChildLiveBitmap) {
  publish(0);
  auto before = fieldMetadataMemory.liveViews.load();
  auto req = request();
  req->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(req->mr, Fixture::CHILD, qb::all(), "none");
  auto child = qb::boolean(req->mr, {qb::exists(req->mr, "number")}, {},
      {qb::match(req->mr, "body", "shared")});
  req->topDocs("boolean").countLimit().limit(0).rawQuery() = qb::toParent(req->mr, Fixture::CHILD, child);
  req->execute(root);
  ASSERT_OK(req);
  EXPECT_EQ(3, req->getMatchCount());
  EXPECT_EQ(2, req->getMatchCount("boolean"));
  EXPECT_EQ(before, fieldMetadataMemory.liveViews.load());
}

TEST_F(ScopedRowSpaceTest, preparedPredicateChildKeepsRawMembershipAcrossDeletes) {
  for (bool deleted : {false, true}) {
    if (deleted) publish(0);
    auto before = fieldMetadataMemory.liveViews.load();
    auto req = request();
    auto inner = qb::toParent(req->mr, "deep", qb::all(), "none");
    auto predicate = qb::boolean(req->mr, {inner}, {}, {}, {qb::match(req->mr, "label", "a")});
    auto join = qb::toParent(req->mr, Fixture::CHILD, predicate, "none");
    req->topDocs().countLimit().limit(0).rawQuery() = join;
    req->topDocs("cached").allQuery().countLimit().limit(0).filter(join);
    req->execute(root);
    ASSERT_OK(req);
    EXPECT_EQ(deleted ? 3 : 4, req->getMatchCount());
    EXPECT_EQ(deleted ? 3 : 4, req->getMatchCount("cached"));
    EXPECT_EQ(before, fieldMetadataMemory.liveViews.load());
  }
}

TEST_F(ScopedRowSpaceTest, transformedDomainsListFacetAndProjectDistinctOwners) {
  auto req = request();
  auto& children = req->topDocs().allQuery().countLimit().fields({"number", "payload"});
  children.rawOp().domain.emplace().to = Fixture::CHILD;
  children.facet("labels", "label").mincount(1);
  auto& owners = children.topDocs("owners").allQuery().countLimit().fields({"number"});
  owners.rawOp().domain.emplace().to = "/";
  req->topDocs("roots").allQuery().countLimit().limit(0);
  req->execute(root);
  ASSERT_OK(req);
  EXPECT_EQ(8, req->getMatchCount());
  EXPECT_EQ(12, req->getMatchCount("roots"));
  const auto* list = req->docList();
  auto* labels = (*list->ops.find("labels"))->facetResult();
  EXPECT_EQ((std::vector<int64_t>{4, 2}), std::vector<int64_t>(labels->counts.begin(), labels->counts.end()));
  EXPECT_EQ(4, *(*list->ops.find("owners"))->docList()->found);
}

TEST_F(ScopedRowSpaceTest, transformedDomainFeedsAChildFieldFacetDirectly) {
  auto req = request();
  auto& facet = req->facet("labels", "label").mincount(1);
  facet.rawOp().domain.emplace().to = Fixture::CHILD;
  req->execute(root);
  ASSERT_OK(req);
  auto* labels = (*req->responses[0]->proto.ops.find("labels"))->facetResult();
  EXPECT_EQ((std::vector<int64_t>{4, 2}), std::vector<int64_t>(labels->counts.begin(), labels->counts.end()));
}

TEST_F(ScopedRowSpaceTest, inheritedOwnerDomainExcludesWholeFamilies) {
  auto req = request();
  auto& owners = req->topDocs().matchQuery("number", "101").countLimit().limit(0);
  auto& children = owners.topDocs("children").allQuery().countLimit().fields({"number"});
  children.rawOp().domain.emplace().to = Fixture::CHILD;
  req->execute(root, false);
  ASSERT_OK(req);
  EXPECT_EQ(3, req->getMatchCount());
  auto* childList = (*req->docList()->ops.find("children"))->docList();
  EXPECT_EQ(4, *childList->found);
}

TEST_F(ScopedRowSpaceTest, transformedResetKeepsItsRootBucket) {
  auto req = request();
  auto& roots = req->facet("numbers", "number").mincount(1);
  auto& children = roots.topDocs("children").allQuery().countLimit().limit(0);
  auto& domain = children.rawOp().domain.emplace();
  domain.to = Fixture::CHILD;
  auto* query = (api::Query*)req->mr.allocate(sizeof(api::Query), alignof(api::Query));
  new (query) api::Query(qb::all());
  domain.query = query;
  req->execute(root, false);
  ASSERT_OK(req);
  auto* numbers = (*req->responses[0]->proto.ops.find("numbers"))->facetResult();
  auto& lists = std::get<api::ArrVal>((**numbers->ops.find("children")).kind).v;
  ASSERT_EQ(3u, lists.size());
  EXPECT_EQ(4, *lists[0].docList()->found);
  EXPECT_EQ(0, *lists[1].docList()->found);
  EXPECT_EQ(4, *lists[2].docList()->found);
}

TEST_F(ScopedRowSpaceTest, transformedResetReappliesOwnerFiltersInTheirOwnSpace) {
  auto req = request();
  auto& owners = req->topDocs().matchQuery("number", "101").matchFilter("number", "103").countLimit().limit(0);
  auto& labels = owners.facet("labels", "label").mincount(0);
  auto& children = labels.topDocs("children").allQuery().countLimit().limit(0);
  auto& domain = children.rawOp().domain.emplace();
  domain.to = Fixture::CHILD;
  domain.apply_parent_filters = true;
  auto* query = (api::Query*)req->mr.allocate(sizeof(api::Query), alignof(api::Query));
  new (query) api::Query(qb::all());
  domain.query = query;
  req->execute(root, false);
  ASSERT_OK(req);
  EXPECT_EQ(0, req->getMatchCount());
  auto* facets = (*req->docList()->ops.find("labels"))->facetResult();
  auto& lists = std::get<api::ArrVal>((**facets->ops.find("children")).kind).v;
  ASSERT_EQ(1u, lists.size());
  EXPECT_EQ(4, *lists[0].docList()->found);
}

TEST_F(ScopedRowSpaceTest, jsonSurfaceCrossesViewsAndProjectsToRoot) {
  auto req = request();
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"json({"ops":{
    "q":{"top_docs":{"query":{"to_parent":{"path":"items","score":"none",
      "query":{"match":{"field":"body","val":"shared"}}}},"count_limit":-1,"limit":0}},
    "children":{"domain":{"to":"items"},"top_docs":{"count_limit":-1,"limit":0,
      "ops":{"owners":{"domain":{"to":"/"},"top_docs":{"count_limit":-1,"limit":0}}}}}
  }})json", req->mr));
  req->execute(root);
  ASSERT_OK(req);
  EXPECT_EQ(4, req->getMatchCount());
  EXPECT_EQ(8, req->getMatchCount("children"));
  EXPECT_EQ(4, *(*req->docList("children")->ops.find("owners"))->docList()->found);
}

TEST_F(ScopedRowSpaceTest, relationsAreSharedByPairInEitherRequestView) {
  for (auto primary : {root, root->view(Fixture::CHILD)}) {
    auto req = request();
    req->reader = primary;
    auto& many = req->pinView(Fixture::CHILD);
    auto& one = req->pinView("");
    auto& relation = req->ownershipRelation(*many, *one);
    EXPECT_EQ(&relation, &req->ownershipRelation(*many, *one));
    auto& deep = req->pinView("items.deep");
    EXPECT_NE(&relation, &req->ownershipRelation(*deep, *one));
    RowRelation::Mapping expand{&relation, RowRelation::Direction::EXPAND};
    RowRelation::Mapping project{&relation, RowRelation::Direction::PROJECT};
    EXPECT_EQ(one.get(), &expand.source());
    EXPECT_EQ(many.get(), &project.source());
  }
}

TEST_F(ScopedRowSpaceTest, noneReducerKeepsReadsStableAcrossNextAndAdvance) {
  auto many = root->view(Fixture::CHILD);
  MemPool pool;
  Query::Context context(pool, *many);
  OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
  AllQuery all;
  auto* weight = all.createWeight(context, 0);
  for (bool seek : {false, true}) {
    auto* input = weight->scorerSupplier(pool, many->segments()[0]);
    auto* supplier = relation.reduce(pool, 0, *input, nullptr, RowRelation::Reduction::NONE, 2.0f);
    auto* scorer = buildScorerForTests(pool, *supplier, 4);
    ASSERT_NE(nullptr, scorer);
    EXPECT_EQ(0, scorer->next());
    for (int repeat = 0; repeat < 3; repeat++) {
      EXPECT_EQ(0, scorer->docId());
      EXPECT_FLOAT_EQ(2.0f, scorer->score());
    }
    EXPECT_EQ(2, seek ? scorer->advance(1) : scorer->next());
    for (int repeat = 0; repeat < 3; repeat++) {
      EXPECT_EQ(2, scorer->docId());
      EXPECT_FLOAT_EQ(2.0f, scorer->score());
    }
    EXPECT_EQ(PostingsReader::END, seek ? scorer->advance(3) : scorer->next());
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }
}

TEST_F(ScopedRowSpaceTest, reducersShareFilterCacheMembership) {
  uint64_t hits = 0;
  for (auto reducer : {"none", "max"}) {
    auto req = request();
    req->topDocs().allQuery().countLimit().limit(0).filter(qb::toParent(req->mr,
        Fixture::CHILD, qb::match(req->mr, "label", "a"), reducer));
    req->execute(root);
    ASSERT_OK(req);
    EXPECT_EQ(4, req->getMatchCount());
    if (reducer == std::string_view("max")) {
      EXPECT_GT(cache->counters().hits, hits);
    }
    hits = cache->counters().hits;
  }
}

TEST_F(ScopedRowSpaceTest, warmedJoinAndChildFiltersStayCorrectAfterDeletes) {
  for (bool afterDelete : {false, true}) {
    if (afterDelete) publish(0);
    for (int repeat = 0; repeat < 2; repeat++) {
      auto owners = request();
      owners->topDocs().allQuery().countLimit().limit(0).filter(qb::toParent(owners->mr,
          Fixture::CHILD, qb::match(owners->mr, "label", "a"), "none"));
      owners->execute(root);
      ASSERT_OK(owners);
      EXPECT_EQ(afterDelete ? 3 : 4, owners->getMatchCount());
      auto children = request();
      children->topDocs().allQuery().countLimit().limit(0).matchFilter("label", "a");
      children->execute(root->view(Fixture::CHILD));
      ASSERT_OK(children);
      EXPECT_EQ(afterDelete ? 3 : 4, children->getMatchCount());
    }
  }
  EXPECT_GT(cache->counters().hits, 0u);
}

TEST_F(ScopedRowSpaceTest, relativeJoinAndExpansionPathsStartAtTheCurrentView) {
  auto req = request();
  req->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(req->mr, "deep", qb::all());
  auto& deeper = req->topDocs("deeper").allQuery().countLimit().limit(0);
  deeper.rawOp().domain.emplace().to = "deep";
  req->execute(root->view(Fixture::CHILD));
  ASSERT_OK(req);
  EXPECT_EQ(8, req->getMatchCount());
  EXPECT_EQ(8, req->getMatchCount("deeper"));
}

TEST_F(ScopedRowSpaceTest, childWildcardEmitUsesLocalKeysAndOwnStoredValues) {
  auto req = request();
  req->topDocs().matchQuery("label", "a").countLimit().fields({"*"});
  req->execute(root->view(Fixture::CHILD));
  ASSERT_OK(req);
  ASSERT_EQ(4u, req->getDocs().size());
  EXPECT_TRUE(docEquals(flatdoc("number", (int64_t)11, "label", "a", "payload", "child zero"), req->getDocs()[0]));
  for (const auto& doc : req->getDocs()) {
    for (const auto& value : doc) EXPECT_EQ(std::string::npos, value.name.find('.'));
  }
}

TEST_F(ScopedRowSpaceTest, unknownFieldsAndPathsReportTheCurrentView) {
  for (auto name : {"items.body", "unknown"}) {
    auto req = request();
    req->topDocs().matchQuery(name, "shared");
    req->execute(root->view(Fixture::CHILD));
    ASSERT_FALSE(req->responses.empty());
    auto& response = req->responses[0]->proto;
    ASSERT_TRUE(response.error.has_value());
    EXPECT_NE(std::string_view::npos, response.error->message.find("view 'items'"));
  }
  auto req = request();
  req->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(req->mr, "unknown", qb::all());
  req->execute(root);
  ASSERT_TRUE(req->responses[0]->proto.error.has_value());
  EXPECT_NE(std::string_view::npos, req->responses[0]->proto.error->message.find("Field not found: unknown"));
  EXPECT_EQ(std::string_view::npos, req->responses[0]->proto.error->message.find("view"));
}

TEST_F(ScopedRowSpaceTest, declaredAbsentPathsAreEmptyAndOtherPathsAreRejected) {
  auto empty = request();
  empty->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(empty->mr, "absent", qb::all());
  empty->execute(root);
  ASSERT_OK(empty);
  EXPECT_EQ(0, empty->getMatchCount());
  auto bad = request();
  bad->reader = root;
  EXPECT_THROW(bad->pinView("missing"), RequestError);
  EXPECT_THROW(bad->pinView("number"), RequestError);
}

} // namespace
} // namespace luxir::test
