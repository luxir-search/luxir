// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "test/RowSpaceFixture.h"
#include "luxir/index/SegmentMerger.h"
#include "luxir/reader/ColumnShape.h"
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
    EXPECT_NE(core.rootReader().fieldInfo("number"), child->fieldInfo("number"));
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
  auto* number = child.fieldInfo("number");
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
  TermsEnum terms(pool, child, *child.fieldInfo("body"));
  ASSERT_TRUE(terms.seek("shared"));
  DocsFreqEnum docs(terms);
  EXPECT_EQ(0, docs.nextDoc()); EXPECT_EQ(2, docs.termFreq());
  EXPECT_EQ(2, docs.nextDoc()); EXPECT_EQ(1, docs.termFreq());
  EXPECT_EQ(PostingsReader::END, docs.nextDoc());
  EXPECT_EQ(5, child.fieldInfo("body")->sumTotalTermFreq);
  TermsEnum rootTerms(pool, core->rootReader(), *core->rootReader().fieldInfo("body"));
  EXPECT_FALSE(rootTerms.seek("shared"));
}

TEST_F(RowSpaceTest, childStringColumnKeepsSpaceLocalDensityAndPresence) {
  auto core = open();
  auto& child = *core->spaceReader(Fixture::CHILD);
  auto* label = child.fieldInfo("label");
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
      EXPECT_EQ(OWNERS[child], cursor.owner(child));
      EXPECT_EQ(OWNERS[child], shape->owner(child));
    }
  }
}

TEST_F(RowSpaceTest, openNeedsNoChildRowMapOrScan) {
  RAMDir dir;
  PostingsWriter writer(dir, 1, 1, 1);
  const int32_t children = 100000000;
  writer.addSpace(Fixture::CHILD, children,
                  Fixture::shape(writer, std::vector<int64_t>{children}));
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
  writer.addSpace(Fixture::CHILD, (int32_t)owners.size(), Fixture::shape(writer, ends));
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  const auto& shape = *core.ownership(Fixture::CHILD);
  OwnershipShape::Cursor cursor(shape);
  for (int32_t child = 0; child < (int32_t)owners.size(); child += 7) EXPECT_EQ(owners[child], cursor.owner(child));
  for (int32_t parent : {parents - 1, 0, parents - 2}) {
    EXPECT_EQ((std::pair<int32_t, int32_t>{parent ? (int32_t)ends[parent - 1] : 0, (int32_t)ends[parent]}), shape.range(parent));
  }
}

TEST_F(RowSpaceTest, pathLookupBindsFieldsAndImmediateOwnership) {
  PostingsWriter writer(dir, 1, 1, 3);
  auto items = writer.addSpace("z", 4, Fixture::shape(writer, ENDS));
  Fixture::fields(writer, items, CHILDREN);
  writer.addSpace("z.parts", 2, Fixture::shape(writer, std::vector<int64_t>{1, 1, 2, 2}));
  auto earlier = writer.addSpace("a", 3, Fixture::shape(writer, std::vector<int64_t>{1, 1, 3}));
  Fixture::fields(writer, earlier, ROOTS);
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
    IntColReader column(*reader, *reader->fieldInfo("number"));
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
    writer.addSpace(Fixture::CHILD, 4, Fixture::shape(writer, ends, owners));
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
  writer.addSpace(Fixture::CHILD, 4, shape);
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

TEST_F(RowSpaceTest, multiSpaceMergeFailsBeforeWritingFields) {
  RAMDir dir, output;
  Fixture::write(dir, 1, ROOTS, CHILDREN, ENDS);
  SegmentCore core(dir, 1, 1);
  std::vector<PostingsReader*> readers{&core.rootReader()};
  std::vector<LiveDocs*> live{nullptr};
  PostingsWriter writer(output, 2, 1);
  IndexRamBudget budget;
  Schema schema;
  StoredDictionaryRegistry dictionaries;
  SegmentMerger merger(readers, live, writer, budget, INT64_MAX, 1, 4, schema, dictionaries);
  EXPECT_THROW(merger.merge(), std::invalid_argument);
  std::vector<Directory::FileInfo> files;
  output.listFiles(files);
  EXPECT_TRUE(files.empty());
}

} // namespace
} // namespace luxir::test
