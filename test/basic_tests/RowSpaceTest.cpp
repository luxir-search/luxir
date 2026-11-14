// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "test/RowSpaceFixture.h"
#include "test/CollectionHelper.h"
#include "test/QueryBuild.h"
#include "test/StorageTestUtils.h"
#include "luxir/index/LiveDocsWriter.h"
#include "luxir/search/DomainVariantPlan.h"
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
    schema->fieldTypeMap["number"] = std::make_shared<IntFieldType>("number", FieldType::COLUMN_STORED);
    schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>("body", FieldType::INDEX_DOCS_FREQS, "whitespace");
    schema->fieldTypeMap["label"] = std::make_shared<StrFieldType>("label", FieldType::INDEX_DOCS | FieldType::COLUMN_STORED);
    schema->fieldTypeMap["payload"] = std::make_shared<TextFieldType>("payload", FieldType::STORED, "whitespace");
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
      std::vector<FileDescriptor> files;
      if (previous.empty()) {
        PostingsWriter writer(directory, segment.seg_id, 1, (int32_t)roots.size());
        Fixture::fields(writer, SpaceOrdinal::ROOT, roots, std::vector<uint8_t>{1, 1, 1});
        if (i < 2) {
          auto child = writer.addSpace(Fixture::CHILD, (int32_t)CHILDREN.size(), Fixture::shape(writer, ends));
          Fixture::fields(writer, child, CHILDREN, std::vector<uint8_t>{2, 2, 1});
        }
        writeUnversionedRuns(writer);
        ASSERT_TRUE(writer.finish(nullptr, &files));
      } else {
        auto& old = previous[(size_t)i];
        auto liveName = old.live_gen ? Postings::getLiveDocsFileName(
            Postings::segmentKey(old.seg_id, old.creator_epoch), old.live_gen, old.live_epoch) : std::string{};
        for (const auto& file : old.files) if (file.name != liveName) {
          files.push_back({std::string(file.name), file.size, file.xxh3});
        }
      }
      if (i == deletedBundle) {
        uint64_t word = 13;
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
  EXPECT_EQ(8, child->field("body").stats().maxDoc);
  EXPECT_EQ(6, child->field("body").stats().docsWithField);
  EXPECT_EQ(10, child->field("body").stats().sumTotalTermFreq);
  EXPECT_EQ(9, root->field("body").stats().sumTotalTermFreq);
  EXPECT_NE(child->getOrdMap("label"), root->getOrdMap("label"));
  MemPool pool;
  Query::Context context(pool, *child);
  Similarity::TermStats stats;
  ASSERT_TRUE(context.lookupTermStats("body", "shared", stats));
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
  writer.addSpace(Fixture::CHILD, count, Fixture::shape(writer, std::vector<int64_t>{count}));
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
  EXPECT_EQ(count + 8, child->field("body").stats().maxDoc);
  EXPECT_LT(child->segments()[3].segmentCore().sizeInBytes(), 1024u);
  EXPECT_EQ(8, root->view(Fixture::CHILD)->maxDoc());
}

TEST_F(ScopedRowSpaceTest, refusesChildLivenessAndBirthVersions) {
  auto req = request();
  req->topDocs().allQuery().fields({"_version_"});
  req->execute(root->view(Fixture::CHILD));
  EXPECT_FALSE(req->ok());
  EXPECT_EQ(ErrorKind::INTERNAL, req->errorKind());
  EXPECT_EQ("child_version_unavailable", req->errorCode());
  publish(2); // Deletes in an absent path's bundle do not affect the child view.
  EXPECT_EQ(8, root->view(Fixture::CHILD)->liveDocs());
  publish(0);
  try {
    root->view(Fixture::CHILD);
    FAIL() << "child rows with root deletes must be refused";
  } catch (const ApiError& error) {
    EXPECT_EQ(ErrorKind::INTERNAL, error.kind);
    EXPECT_NE(std::string::npos, std::string(error.what()).find("root deletes"));
  }
}

} // namespace
} // namespace luxir::test
