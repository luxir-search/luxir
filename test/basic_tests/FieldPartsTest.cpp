// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <tuple>

#include "luxir/index/NormsWriter.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/index/StoredFieldsWriter.h"
#include "luxir/index/handler/StrColHandler.h"
#include "luxir/index/handler/VectorHandler.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/FieldCursor.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/util/geo.h"
#include "test/FieldRecordTest.h"
#include "test/TestUtils.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace luxir::test {
namespace {

using field_parts::Slice;
constexpr auto ROOT = SpaceOrdinal::ROOT;
constexpr int32_t NUM_DOCS = 4;

void checkCorruptRecord(const EncodedFieldRecord& record, std::vector<uint64_t> files) {
  auto bytes = std::span(record.bytes);
  for (size_t i = 0; i < bytes.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_THROW(FieldRecordCodec::decode(bytes.first(i), NUM_DOCS, files, -1), std::runtime_error);
    if (i < record.headerBytes) {
      EXPECT_THROW(FieldRecordCodec::fieldHeader(bytes.first(i), NUM_DOCS), std::runtime_error);
    }
  }

  auto corrupt = record.bytes;
  InputStream prefix(bytes.data(), bytes.data() + record.headerBytes);
  prefix.checkedPackedTerm();
  auto type = prefix.offset();
  prefix.checkedVint();
  auto flags = prefix.offset();
  auto flagsValue = prefix.checkedVint();
  auto flagsEnd = prefix.offset();
  auto mask = prefix.offset();
  prefix.checkedVint();
  auto state = prefix.offset();
  for (auto [offset, bits] : {std::pair{mask, 0x80}, {state, 0x02}, {state, 0x80}}) {
    corrupt = record.bytes;
    corrupt[offset] |= bits;
    EXPECT_THROW(FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS), std::runtime_error);
  }

  corrupt = record.bytes;
  ASSERT_LT((uint8_t)corrupt[type], 128);
  corrupt[type] |= 0x80;
  corrupt.insert(corrupt.begin() + type + 1, 0);
  EXPECT_THROW(FieldRecordCodec::decode(corrupt, NUM_DOCS, files, -1), std::runtime_error);

  for (char level : {2, 4, 5, 6}) {
    corrupt = record.bytes;
    corrupt[flags] = (corrupt[flags] & ~7) | level;
    EXPECT_THROW(FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS), std::runtime_error) << (int)level;
  }
  ASSERT_EQ(FieldType::NUM_TOKENS_EXACT, record.field.header.flags & FieldType::NUM_TOKENS_EXACT);
  for (auto bit : {FieldType::NUM_TOKENS_APPROX, 1 << 11, FieldType::ABSTRACT, FieldType::DERIVED}) {
    corrupt = record.bytes;
    std::array<char, MAX_VLONG_SIZE> encoded;
    auto* end = writeVLong(encoded.data(), flagsValue | bit);
    corrupt.erase(corrupt.begin() + flags, corrupt.begin() + flagsEnd);
    corrupt.insert(corrupt.begin() + flags, encoded.data(), end);
    if (bit == FieldType::NUM_TOKENS_APPROX) {
      EXPECT_EQ(flagsValue | bit, (uint32_t)FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS).flags);
    } else {
      EXPECT_THROW(FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS), std::runtime_error) << bit;
    }
  }

  corrupt = record.bytes;
  corrupt.push_back(0);
  EXPECT_EQ(FieldRecordCodec::fieldHeader(bytes, NUM_DOCS), FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS));
  EXPECT_THROW(FieldRecordCodec::decode(corrupt, NUM_DOCS, files, -1), std::runtime_error);

  InputStream terms(bytes.data() + record.headerBytes, bytes.data() + bytes.size());
  ASSERT_LE(terms.checkedVint(), 3u);  // Singleton run tag.
  auto dictionaryLength = record.headerBytes + terms.offset();
  ASSERT_LT(terms.checkedVint(), 128u);
  auto trieLength = record.headerBytes + terms.offset();
  ASSERT_LT(terms.checkedVint(), 128u);
  for (auto [offset, length, reason] : {
       std::tuple{dictionaryLength, 0, "term run is empty"}, {trieLength, 0, "trie root outside trie"}}) {
    corrupt = record.bytes;
    corrupt[offset] = (char)length;
    try {
      FieldRecordCodec::decode(corrupt, NUM_DOCS, files, -1);
      ADD_FAILURE() << "Expected corruption: " << reason;
    } catch (const std::runtime_error& e) {
      EXPECT_TRUE(std::string_view(e.what()).contains(reason)) << e.what();
    }
  }

  terms.checkedVlong();  // Trie root.
  terms.checkedVint();  // Dictionary file.
  terms.checkedVlong();  // Dictionary start.
  auto docsStart = record.headerBytes + terms.offset();
  terms.checkedVint();  // Docs file.
  terms.checkedVlong();  // Docs start.
  ASSERT_GT(terms.checkedVlong(), 0u);
  auto positionsStart = record.headerBytes + terms.offset();
  terms.checkedVint();  // Positions file.
  terms.checkedVlong();  // Positions start.
  ASSERT_GT(terms.checkedVlong(), 0u);
  auto positionsEnd = record.headerBytes + terms.offset();
  corrupt = record.bytes;
  corrupt.erase(corrupt.begin() + positionsStart, corrupt.begin() + positionsEnd);
  corrupt.insert(corrupt.begin() + positionsStart, bytes.begin() + docsStart, bytes.begin() + positionsStart);
  try {
    FieldRecordCodec::decode(corrupt, NUM_DOCS, files, -1);
    ADD_FAILURE() << "Expected overlapping posting extents to be rejected";
  } catch (const std::runtime_error& e) {
    EXPECT_TRUE(std::string_view(e.what()).contains("overlapping data slices")) << e.what();
  }

  const auto& dictionary = record.field.terms.dictionaryBlocks();
  files[dictionary.location.filenum()] = dictionary.end().offset() - 1;
  EXPECT_THROW(FieldRecordCodec::decode(bytes, NUM_DOCS, files, -1), std::runtime_error);
  files[dictionary.location.filenum()] = 0;
  EXPECT_THROW(FieldRecordCodec::decode(bytes, NUM_DOCS, files, -1), std::runtime_error);
}

void checkNoncanonicalValueCount(const EncodedFieldRecord& record) {
  auto corrupt = record.bytes;
  InputStream input(corrupt.data(), corrupt.data() + record.headerBytes);
  input.checkedPackedTerm();
  input.checkedVint();  // Type.
  input.checkedVint();  // Flags.
  input.checkedVint();  // Part mask.
  auto state = input.offset();
  input.skip(1);
  auto docs = input.checkedVint();
  ASSERT_LT(docs, 128u);
  constexpr uint8_t EXPLICIT_NUM_VALUES = 0x80;
  corrupt[state] |= EXPLICIT_NUM_VALUES;
  corrupt.insert(corrupt.begin() + input.offset(), (char)docs);
  EXPECT_THROW(FieldRecordCodec::fieldHeader(corrupt, NUM_DOCS), std::runtime_error);
}

TEST(FieldRecordCodecTest, rejectsInconsistentColumnAtWrite) {
  using namespace field_parts;
  MemPool pool;
  IndexFieldInfo field;
  auto& header = field.header;
  header = {.name = PackedTerm(pool, "field"), .type = FieldType::INT,
                           .flags = FieldType::COLUMN_STORED, .parts = {},
                           .docsWithField = 1, .numValues = 1, .indexedTotals = {}, .bounds = ScalarBounds{0, 0}};
  RAMFile encoded("");
  OutputStream out(&encoded);
  EXPECT_THROW(FieldRecordCodec::encode(out, field, 1), std::logic_error);
  header.parts.add(FieldPart::COLUMN);
  header.numValues = 0;
  EXPECT_THROW(FieldRecordCodec::encode(out, field, 1), std::logic_error);
  header.numValues = 2;
  EXPECT_THROW(FieldRecordCodec::encode(out, field, 1), std::logic_error);
  EXPECT_THROW(field.setColumn(WrittenColumn{BytesColumn{}, 1, WrittenPresence{1, {}}, Slice{}, Slice{}, Bounds()}),
               std::logic_error);
  EXPECT_EQ(0u, out.size());
  out.close();
}

TEST(FieldRecordCodecTest, canonicalVlongStopsAtOverflow) {
  std::array<char, MAX_VLONG_SIZE + 1> bytes;
  bytes.fill((char)VARINT_CONTINUATION);
  bytes.back() = 7;
  InputStream input(bytes.data(), bytes.data() + bytes.size());
  auto result = input.readCanonicalVlong(UINT64_MAX);
  ASSERT_FALSE(result);
  EXPECT_STREQ("vint overflow", result.error());
  EXPECT_EQ(MAX_VLONG_SIZE, input.offset());
  EXPECT_EQ(7, input.readByte());
}

TEST(FieldRecordCodecTest, presentEmptySliceAtFileEnd) {
  RAMDir dir;
  PostingsWriter writer(dir, 1, 1, 1);
  auto& field = writer.addField(ROOT, "empty");
  field.header.type = FieldType::STRING;
  field.header.flags = FieldType::COLUMN_STORED;
  auto offset = Postings::LUXIR_HEADER.size();
  field.setColumn(WrittenColumn{field_parts::BytesColumn{{{0, offset}, 0}, {}}, 1, {1, {}}, {}, {}, {}});
  EncodedFieldRecord record(field, 1);
  std::array<uint64_t, 1> files{offset};
  record.check(1, files);
  auto decoded = FieldRecordCodec::decode(record.bytes, 1, files, -1);
  ASSERT_TRUE(decoded->bytesColumn());
  EXPECT_EQ(offset, decoded->bytesColumn()->values.location.offset());
  EXPECT_EQ(0u, decoded->bytesColumn()->values.bytes);
}

TEST(FieldRecordCodecTest, zeroDocRecordIsRejectedAndOmitted) {
  RAMDir dir;
  PostingsWriter writer(dir, 1, 1, 2);
  auto& field = writer.addField(ROOT, "empty");
  field.header.type = FieldType::STRING;
  auto presence = PresenceWriter(writer.pool, writer, ROOT).finish();
  EXPECT_FALSE(presence.bitmap);
  field.setPresence(presence);
  RAMFile record("");
  OutputStream out(&record);
  EXPECT_THROW(FieldRecordCodec::encode(out, field, 2), std::logic_error);
  out.close();
  // Name, type, flags, part set, state, and docsWithField.
  const std::array bytes{(char)1, 'f', (char)FieldType::STRING, (char)0, (char)0, (char)0, (char)0};
  EXPECT_THROW(FieldRecordCodec::fieldHeader(bytes, 2), std::runtime_error);
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());
  SegmentCore core(dir, 1, 1);
  EXPECT_EQ(0, core.rootReader().numFields());
}

TEST(FieldRecordCodecTest, zeroDocFieldWithPartsIsWriterMisuse) {
  RAMDir dir;
  PostingsWriter writer(dir, 1, 1, 1);
  auto& field = writer.addField(ROOT, "text");
  field.header.type = FieldType::TEXT;
  field.setNorms({seg_location(0, IndexFormat::HEADER.size()), field_parts::NormsMode::FLAT});
  writeUnversionedRuns(writer);
  EXPECT_THROW(writer.finish(), std::logic_error);
}

TEST(FieldRecordCodecTest, intervalValuesRequireBounds) {
  MemPool pool;
  IndexFieldInfo field;
  field.header.name = PackedTerm(pool, "range");
  field.header.type = FieldType::INTERVAL;
  field.header.flags = FieldType::COLUMN_STORED | FieldType::MULTI_VALUED;
  field.setColumn(WrittenColumn{field_parts::NumberColumn{}, 2, {1, {}}, {{0, 8}, 32}, {}, {}});
  RAMFile file("");
  OutputStream out(&file);
  try {
    FieldRecordCodec::encode(out, field, 1);
    ADD_FAILURE() << "Expected missing interval bounds to be rejected";
  } catch (const std::logic_error& error) {
    EXPECT_TRUE(std::string_view(error.what()).contains("interval values require interval bounds"));
  }
  out.close();
}

TEST(FieldRecordCodecTest, shortCachePage) {
  for (int count : {1, 65}) {
    RAMDir dir;
    PostingsWriter writer(dir, 1, 1, 1);
    for (int i = 0; i < count; ++i) {
      auto output = writer.getOutputStream();
      IntColWriter number(*output, false);
      number.addInt64(i);
      auto written = IntColWriter::placeColumn(writer, std::move(output), number.finish());
      auto& field = writer.addField(ROOT, std::format("field{:02}", i));
      field.header.type = FieldType::INT;
      field.header.flags = FieldType::COLUMN_STORED;
      field.setColumn(WrittenColumn{written.column, 1, {1, {}}, {}, {}, written.bounds});
    }
    writeUnversionedRuns(writer);
    ASSERT_TRUE(writer.finish());
    SegmentCore core(dir, 1, 1);
    auto& reader = core.rootReader();
    auto record = reader.decodeField(count - 1);
    ASSERT_NE(nullptr, reader.openNumericLeafMeta(*record));
    record.reset();
    auto stats = reader.fieldCacheStats();
    EXPECT_EQ(1u, stats.pages);
    EXPECT_EQ(1u, stats.entries);
    EXPECT_EQ(1u, stats.openedEntries);
    EXPECT_EQ(16u, stats.pageBytes);
    EXPECT_EQ(8u, stats.openedPageBytes);
  }
}

TEST(FieldRecordCodecTest, pointsDirectoryLengths) {
  // Three leaves, on an 8-byte boundary.
  constexpr uint64_t start = 16;
  field_parts::Points points{{0, start}, 8, 2};
  for (auto [type, values, length] : {std::tuple{FieldType::INT, 6, 124}, {FieldType::GEO_POINT, 6, 128},
                                      {FieldType::INTERVAL, 12, 184}}) {
    EXPECT_EQ(3u, field_parts::pointLeafCount(points, type, values));
    EXPECT_EQ((Slice{{0, start + 8}, (uint64_t)length}), field_parts::pointsDirectory(points, type, values));
  }
}

#if defined(__unix__) || defined(__APPLE__)
size_t mappingPageSize() {
  auto pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) throw std::runtime_error("unable to determine mapping page size");
  return (size_t)pageSize;
}

// Protect the backing bytes so a dereference through a retained view is checked.
class ProtectedInputFile : public InputFile {
  char* data;
  size_t bytes;
  size_t pageSize = mappingPageSize();

public:
  explicit ProtectedInputFile(std::string_view contents) : bytes(contents.size()) {
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("mmap failed");
    data = (char*)mapping;
    memcpy(data, contents.data(), bytes);
  }
  ~ProtectedInputFile() override { munmap(data, bytes); }
  size_t size() override { return bytes; }
  HeldRange acquire(ByteRange range) override {
    checkRange(range, bytes);
    return {data + range.offset, range.length};
  }
  bool holdsFree() const override { return true; }

  void protect() {
    if (mprotect(data, bytes, PROT_NONE) != 0) throw std::runtime_error("mprotect failed");
  }
  void allow(uint64_t offset, uint64_t length) {
    if (length == 0) return;
    uint64_t start = offset - offset % pageSize;
    if (mprotect(data + start, offset + length - start, PROT_READ) != 0) {
      throw std::runtime_error("mprotect failed");
    }
  }
};

class ProtectedDirectory : public DelegatingDirectory {
  std::map<std::string, std::shared_ptr<ProtectedInputFile>, std::less<>> inputs;
  const std::string segment = Postings::segmentKey(1, 1);
  bool sealed = false;

public:
  using DelegatingDirectory::DelegatingDirectory;

  std::shared_ptr<InputFile> openFile(std::string_view name, OpenOptions options) override {
    if (sealed) throw std::runtime_error("unexpected segment file open");
    auto source = delegate->openFile(name, options);
    if (!source) return nullptr;
    auto hold = source->acquire({0, source->size()});
    auto file = std::make_shared<ProtectedInputFile>(std::string_view(hold.bytes()));
    inputs.emplace(name, file);
    return file;
  }
  void protect() {
    sealed = true;
    for (auto& [name, file] : inputs) file->protect();
  }
  void allow(Slice slice) {
    inputs.at(Postings::getIndexFileName(segment, slice.location.filenum()))
        ->allow(slice.location.offset(), slice.bytes);
  }
};

void checkRecordAcquisitions(std::shared_ptr<Directory> source, const field_parts::NumberColumn& number,
                             const field_parts::Stored& stored, bool numeric) {
  ProtectedDirectory dir(std::move(source));
  SegmentCore core(dir, 1, 1);
  auto& postings = core.rootReader();
  auto range = core.catalogRange();
  dir.protect();
  dir.allow({seg_location(0, range.offset), range.length});
  for (int32_t ord = 0; ord < postings.numFields(); ord++) {
    auto fieldHeader = postings.fieldHeader(ord);
    const auto* field = postings.fieldInfo(ord);
    ASSERT_NE(nullptr, field);
    EXPECT_EQ(fieldHeader, recordHeader(*field));
  }
  EXPECT_EQ(0u, postings.fieldCacheStats().openedPages);
  const auto* numInfo = postings.fieldInfo("number");
  const auto* storedInfo = postings.fieldInfo("_stored_");
  ASSERT_NE(nullptr, numInfo);
  ASSERT_NE(nullptr, storedInfo);

  const field_parts::ScalarBounds* bounds = nullptr;
  const NumColumnFormat::LeafMeta* leaves = nullptr;
  if (numeric) {
    bounds = numInfo->scalarBounds();
    ASSERT_NE(nullptr, bounds);
    EXPECT_EQ(0, bounds->min);
    EXPECT_EQ(30001, bounds->max);
    EXPECT_EQ(0u, postings.fieldCacheStats().openedEntries);
    dir.protect();
    dir.allow(number.index());
    leaves = postings.openNumericLeafMeta(*numInfo);
    ASSERT_NE(nullptr, leaves);
    EXPECT_EQ(30001, leaves->leafZone(0).max);
  }

  dir.protect();
  dir.allow(stored.names());
  const auto* metadata = postings.openStoredMetadata(*storedInfo);
  ASSERT_NE(nullptr, metadata);
  ASSERT_EQ(1u, metadata->fieldNames().size());
  EXPECT_EQ("original", std::string_view(metadata->fieldNames().front()));
  EXPECT_EQ(numeric ? 2u : 1u, postings.fieldCacheStats().openedEntries);

  dir.protect();
  if (auto dictionary = stored.dictionary()) dir.allow(dictionary);
  const auto* dictionary = postings.openStoredDictionary(*storedInfo->stored(), *metadata);
  EXPECT_EQ(stored.dictionary().has_value(), dictionary != nullptr);

  dir.protect();
  EXPECT_EQ(bounds, numInfo->scalarBounds());
  EXPECT_EQ(leaves, postings.openNumericLeafMeta(*numInfo));
  EXPECT_EQ(metadata, postings.openStoredMetadata(*storedInfo));
  EXPECT_EQ(dictionary, postings.openStoredDictionary(*storedInfo->stored(), *metadata));
}
#endif

class FieldPartsTest : public ::testing::TestWithParam<bool> {
protected:
  static void checkSlices(SegmentCore& core, std::initializer_list<Slice> slices) {
    Slice previous;
    for (auto slice : slices) {
      ASSERT_FALSE(slice.location.isNull());
      ASSERT_EQ(slice.bytes, core.slice(slice).size());
      if (previous) {
        EXPECT_EQ(previous.location.filenum(), slice.location.filenum());
        EXPECT_LE(previous.end().offset(), slice.location.offset());
      }
      previous = slice;
    }
  }

  static void checkMono(SegmentCore& core, Slice slice, std::span<const int64_t> values) {
    checkSlices(core, {slice});
    MonoReader reader(core, slice, values.size());
    EXPECT_EQ(slice.bytes, reader.sizeInBytes());
    for (size_t i = 0; i < values.size(); ++i) EXPECT_EQ(values[i], reader.valueAt(i));
  }

  static InputStream openSlice(SegmentCore& core, Slice slice) {
    auto bytes = core.slice(slice);
    return {bytes.data(), bytes.data() + bytes.size()};
  }

  template <class Reader>
  static void checkPoints(SegmentCore& core, const DecodedFieldRecord& field,
                          const field_parts::Points& part) {
    auto leaves = part.leaves();
    auto directory = field.pointsDirectory();
    auto count = field.pointLeafCount();
    checkSlices(core, {leaves, directory});
    ASSERT_NE(nullptr, field.points());
    EXPECT_EQ(*field.points(), part);
    ASSERT_EQ(0u, leaves.location.offset() % MAX_ALIGN);
    auto input = openSlice(core, {leaves.location, leaves.bytes + directory.bytes});
    Reader sliced(input, 0, leaves.bytes, directory.bytes);
    Reader current(core.rootReader(), field);
    EXPECT_EQ(count, sliced.leafCount());
    EXPECT_EQ(part.maxPointsPerLeaf, sliced.maxPointsPerLeaf());
    EXPECT_EQ(leaves.bytes + directory.bytes, sliced.sizeInBytes());
    EXPECT_EQ(NUM_DOCS, sliced.pointCount());
    for (uint32_t i = 0; i < count; ++i) {
      EXPECT_EQ(current.decodeLeaf(i), sliced.decodeLeaf(i));
    }
  }
};

TEST_P(FieldPartsTest, IndexAndColumnSlices) {
  bool alternate = GetParam();
  auto dir = std::make_shared<RAMDir>();
  Inverter inverter(*dir, 1, 1);
  auto& writer = inverter.getPostingsWriter();
  auto type = std::make_shared<StrFieldType>("bytes", FieldType::COLUMN_STORED);
  handler::StrColHandler bytesWriter(inverter, "bytes", type, ROOT);
  auto vectorType = std::make_shared<VectorFieldType>(
      "vec", 2, FieldType::COLUMN_STORED | FieldType::MULTI_VALUED);
  handler::VectorHandler vectorWriter(inverter, "vec", vectorType, ROOT);
  std::array<float, 2> vec{3.0f, 4.0f};
  for (int32_t doc = 0; doc < NUM_DOCS; ++doc) {
    inverter.startDoc();
    if (doc == 0) {
      bytesWriter.index(inverter, std::string_view("red"));
      vectorWriter.index(inverter, coerce::scalarVal(api::ArrFloat{.v = vec}));
    }
    if (doc == 2) bytesWriter.index(inverter, alternate ? std::string_view("blue") : std::string_view("tan"));
    inverter.finishDoc();
  }
  writer.setMaxDoc(NUM_DOCS);
  auto addField = [&](std::string_view name, FieldType::Type type, int32_t flags) -> auto& {
    auto& info = writer.addField(ROOT, name);
    info.header.type = type;
    info.header.flags = flags;
    return info;
  };
  vectorWriter.finishIndexing(inverter);
  auto vectors = vectorWriter.writeColumn(inverter);
  auto& vectorField = addField("vec", FieldType::VECTOR, vectorType->segmentFlags());
  vectorField.setColumn(vectors);
  auto bytes = bytesWriter.writeColumn(inverter);
  auto& bytesField = addField("bytes", FieldType::STRING, type->segmentFlags());
  bytesField.setColumn(bytes);

  Stream normValues;
  DocStream normDocs(inverter.pool);
  int32_t normCount = alternate ? 1 : NUM_DOCS;
  for (int32_t i = 0; i < normCount; ++i) {
    normValues.writeByte(inverter.pool, (uint8_t)(i + 2));
    normDocs.addDoc(inverter.pool, i);
  }
  auto prepared = NormsWriter::prepare(inverter.pool, writer, ROOT, normValues, normDocs, normCount);

  TextFieldType textType("text", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::NUM_TOKENS_EXACT
      | FieldType::ABSTRACT | FieldType::DERIVED);
  EXPECT_EQ(FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::NUM_TOKENS_EXACT,
            textType.segmentFlags());
  auto& termsField = addField("text", FieldType::TEXT, textType.segmentFlags());
  TextWriter text(writer);
  text.startField(ROOT, termsField);
  text.setNorms(prepared.textView());
  auto term = writer.copyTerm("word");
  text.startTerm(term);
  for (int32_t doc = 0; doc < normCount; ++doc) {
    text.startDoc(doc);
    text.addPositionDelta(1);
    text.addPositionDelta(2);
    text.endDoc(doc);
  }
  text.endTerm(term);
  auto terms = text.endField();
  termsField.setPostings(terms);
  auto norms = NormsWriter::writeValues(inverter.pool, writer, prepared, normDocs);
  termsField.setPresence(prepared.presence);
  ASSERT_TRUE(norms);
  termsField.setNorms(*norms);

  auto& plain = addField("no_positions", FieldType::TEXT, FieldType::INDEX_DOCS_FREQS | FieldType::OMIT_NORMS);
  TextWriter noPositions(writer);
  noPositions.startField(ROOT, plain);
  noPositions.startTerm(term);
  for (int32_t doc = 0; doc < NUM_DOCS; ++doc) noPositions.addDoc(doc, 2);
  noPositions.endTerm(term);
  plain.setPostings(noPositions.endField());
  plain.setPresence({NUM_DOCS, {}});

  auto& ordField = addField("ord", FieldType::STRING,
      FieldType::INDEX_DOCS | FieldType::COLUMN_STORED | (alternate ? FieldType::MULTI_VALUED : 0));
  TextWriter ordTerms(writer);
  ordTerms.startField(ROOT, ordField);
  OrdCollector ords(inverter.pool, NUM_DOCS);
  for (int32_t n = 1; n <= 2; ++n) {
    auto key = writer.copyTerm(n == 1 ? "alpha" : "beta");
    ordTerms.startTerm(key);
    for (int32_t doc = 0; doc < NUM_DOCS; ++doc) {
      if ((doc % 2) + 1 != n && !(alternate && doc == 0)) continue;
      ords.add(doc, n);
      ordTerms.addDoc(doc, 1);
    }
    ordTerms.endTerm(key);
  }
  ordField.setPostings(ordTerms.endField());
  using Override = OrdColWriterSettings::FormatOverride;
  auto old = OrdColWriterSettings::setFormatOverrideForTests(alternate ? Override::PREDICTED : Override::DIRECT);
  auto restore = scope_guard([&] { OrdColWriterSettings::setFormatOverrideForTests(old); });
  OrdColWriter ordWriter(inverter.pool, writer, ROOT, ordField.header.flags, ords);
  auto ord = ordWriter.finish(2);
  ordField.setColumn(ord);
  EXPECT_EQ(NUM_DOCS, ordField.header.docsWithField);
  EXPECT_EQ(alternate ? NUM_DOCS + 1 : NUM_DOCS, ordField.header.numValues);

  size_t pageSize = 4096;
#if defined(__unix__) || defined(__APPLE__)
  pageSize = mappingPageSize();
#endif
  auto separatePages = [&] {
    auto padding = writer.getOutputStream();
    std::string zeros(pageSize, '\0');
    padding->write(zeros.data(), zeros.size());
    writer.finishStream(std::move(padding));
  };
  separatePages();
  auto numberOut = writer.getOutputStream();
  IntColWriter intWriter(*numberOut, !alternate);
  auto numericValue = [&](int32_t doc) -> int64_t {
    return alternate ? geo::pack(10 * doc, -doc) : 10000 * doc + doc % 2;
  };
  for (int32_t i = 0; i < NUM_DOCS; ++i) intWriter.addInt64(numericValue(i));
  auto numericFinish = intWriter.finish();
  auto numericCheckoutBytes = numberOut.pos();
  auto number = IntColWriter::placeColumn(writer, std::move(numberOut), numericFinish);
  separatePages();
  auto& numberField = addField("number", alternate ? FieldType::GEO_POINT : FieldType::INT,
                               FieldType::COLUMN_STORED | FieldType::INDEX_RANGE);
  numberField.setColumn(WrittenColumn{number.column, number.numValues, {NUM_DOCS, {}}, {}, {},
                         !alternate && !number.bounds.empty() ? field_parts::Bounds(number.bounds) : field_parts::Bounds()});
  EXPECT_EQ(NUM_DOCS, numberField.header.numValues);
  EXPECT_EQ(NUM_DOCS, numberField.header.docsWithField);

  auto pointsOut = writer.getOutputStream();
  field_parts::Points points;
  if (alternate) {
    BKDWriter pointWriter(*pointsOut, {.maxPointsPerLeaf = 2});
    std::array<BKDWriter::Point, NUM_DOCS> input{};
    for (int32_t i = 0; i < NUM_DOCS; ++i) input[i] = {10 * i, -i, i};
    auto written = pointWriter.write(input, writer, std::move(pointsOut));
    EXPECT_EQ((field_parts::Bounds(field_parts::GeoBounds{{0, 30}, {-3, 0}})), written.bounds);
    points = written.points;
    numberField.setBounds(written.bounds);
  } else {
    PointsWriter pointWriter(*pointsOut, {.maxPointsPerLeaf = 2});
    for (int32_t i = 0; i < NUM_DOCS; ++i) pointWriter.addPoint(numericValue(i), i);
    points = pointWriter.finish(writer, std::move(pointsOut));
  }
  numberField.setPoints(points);

  auto format = StoredChunkFormat::defaults(alternate ? StoredChunkFormat::ZSTD : StoredChunkFormat::LZ4);
  separatePages();
  std::string dict(4 * pageSize, 'x');
  std::shared_ptr<const StoredCompressionDictionary> dictionary;
  if (alternate) {
    dictionary = std::make_shared<const StoredCompressionDictionary>(
        std::make_shared<const StoredDictionary>(dict), format);
  }
  StoredFieldsWriter storedWriter(writer, format, dictionary);
  storedWriter.addValue(0, "original", "stored value");
  auto stored = storedWriter.finish(NUM_DOCS);
  ASSERT_TRUE(stored);
  writer.addStoredField(ROOT, "_stored_", NUM_DOCS, *stored);
  separatePages();
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());

  const auto& n = number.column;
#if defined(__unix__) || defined(__APPLE__)
  checkRecordAcquisitions(dir, n, *stored, !alternate);
#endif

  SegmentCore core(*dir, 1, 1);
  auto& postings = core.rootReader();
  auto files = segmentFileSizes(*dir, 1, 1);
  for (auto name : {"text", "no_positions", "ord", "number", "bytes", "vec", "_stored_"}) {
    EncodedFieldRecord(*postings.fieldInfo(name), NUM_DOCS).check(NUM_DOCS, files, true);
  }
  if (!alternate) checkCorruptRecord(EncodedFieldRecord(*postings.fieldInfo("text"), NUM_DOCS), files);

  auto vectorWire = EncodedFieldRecord(*postings.fieldInfo("bytes"), NUM_DOCS);
  vectorWire.bytes[vectorWire.field.header.name.memorySize()] = (char)FieldType::VECTOR;
  EXPECT_THROW(FieldRecordCodec::decode(vectorWire.bytes, NUM_DOCS, files, 0), std::runtime_error);

  auto reject = [&](const char* name, auto mutate, std::string_view reason,
                    std::function_ref<void(const DecodedFieldRecord&)> open = [](const DecodedFieldRecord&) {}) {
    SCOPED_TRACE(name);
    EncodedFieldRecord record(*postings.fieldInfo(name), NUM_DOCS);
    mutate(record);
    RAMFile encoded("");
    OutputStream out(&encoded);
    try {
      FieldRecordCodec::encode(out, record.field, NUM_DOCS);
      out.close();
      std::vector<char> bytes(encoded.size());
      encoded.copyTo(bytes.data());
      auto decoded = FieldRecordCodec::decode(bytes, NUM_DOCS, files, -1);
      open(*decoded);
      ADD_FAILURE() << "Expected corruption: " << reason;
    } catch (const std::exception& e) {
      EXPECT_TRUE(std::string_view(e.what()).contains(reason)) << e.what();
    }
  };
  if (alternate) {
    reject("number", [](auto& record) { record.field.header.bounds = field_parts::Bounds(); },
           "geo values require geo bounds");
    reject("text", [](auto& record) { --record.field.docs.bytes; }, "bitmap size");
    reject("ord", [](auto& record) {
      record.field.ord.valuesBytes = 100;
    }, "predicted ordinal payload size");
    reject("ord", [](auto& record) { --record.field.endValueRanks.bytes; }, "end-value ranks index layout");
    reject("bytes", [](auto& record) {
      --record.field.bytes.endOffsets.bytes;
    }, "end offsets index layout");
  } else {
    reject("text", [](auto& record) { record.field.norms.mode = field_parts::NormsMode::SPARSE; },
           "sparse norms require a docs bitmap");
    reject("text", [](auto& record) { record.field.header.parts.add(FieldPart::DOCS); },
           "dense field has a docs bitmap");
    reject("vec", [&](auto& record) { record.field.bytes.endOffsets = vectors.endValueRanks; },
           "vector column must use fixed stride");
    reject("vec", [](auto& record) { record.field.bytes.values.bytes = 3; },
           "vector stride is not a whole vector in the column's encoding");
    reject("vec", [](auto& record) { record.field.bytes.vectorEncoding = (uint8_t)VectorEncoding::SQ8 + 1; },
           "unknown vector encoding");
    reject("bytes", [](auto& record) { record.field.bytes.vectorEncoding = (uint8_t)VectorEncoding::F16; },
           "vector encoding on a non-vector column");
    reject("vec", [](auto& record) {
      record.field.bytes.vectorEncoding = (uint8_t)VectorEncoding::F16;
      record.field.bytes.values.bytes = 7;
    }, "vector stride is not a whole vector in the column's encoding");
    reject("vec", [](auto& record) {
      record.field.bytes.vectorEncoding = (uint8_t)VectorEncoding::SQ8;
      record.field.bytes.values.bytes = vcodec::SQ8_HEADER_BYTES;
    }, "vector stride is not a whole vector in the column's encoding");
    reject("ord", [](auto& record) {
      record.field.header.parts.set(FieldPart::TERMS, false);
      record.field.header.indexedTotals = {};
    }, "ordinal column requires a nonempty terms dictionary");
    reject("text", [](auto& record) {
      record.runs.front().docs.location = {0, 1};
    }, "slice outside file");
    reject("text", [](auto& record) {
      auto& streams = record.runs.front();
      streams.docs = streams.positions;
    }, "overlapping data slices");
    reject("ord", [](auto& record) {
      record.field.header.type = FieldType::ID;
      record.field.header.bounds = std::monostate();
    }, "ID dictionary requires exact bounds");
    reject("bytes", [](auto& record) {
      ++record.field.bytes.values.bytes;
    }, "fixed bytes column size or stride");
    checkNoncanonicalValueCount(EncodedFieldRecord(*postings.fieldInfo("number"), NUM_DOCS));
    reject("number", [](auto& record) {
      record.field.number.blocksBytes = 1024;
    }, "numeric payload size");
    reject("ord", [](auto& record) {
      --record.field.ord.valuesBytes;
    }, "direct ordinal payload size");
    reject("vec", [](auto& record) {
      --record.field.owners.bytes;
    }, "reverse owners index layout");
    reject("_stored_", [](auto& record) {
      --record.field.stored.firstDocs.bytes;
    }, "first docs index layout");
    reject("_stored_", [](auto& record) {
      --record.field.stored.chunkOffsets.bytes;
    }, "chunk offsets index layout");
  }
  reject("number", [](auto& record) { --record.field.points.leavesBytes; }, "points directory alignment");
  reject("number", [](auto& record) {
    // Keep the derived leaf count at two so the reader can check the capacity against the directory.
    record.field.points.maxPointsPerLeaf = 3;
  }, "directory counts disagree with field record", [&](const DecodedFieldRecord& field) {
    if (alternate) BKDReader reader(postings, field);
    else PointsReader reader(postings, field);
  });
  if (alternate) {
    using BitSet = screaming::BitSet;
    ASSERT_TRUE(prepared.presence.bitmap);
    auto bitmap = core.slice(prepared.presence.bitmap);
    ASSERT_GE(bitmap.size(), sizeof(uint16_t) + sizeof(BitSet::BucketDescriptor));
    auto* last = const_cast<char*>(bitmap.data()) + bitmap.size()
        - sizeof(uint16_t) - sizeof(BitSet::BucketDescriptor);
    auto* offset = last + offsetof(BitSet::BucketDescriptor, offset);
    uint32_t saved;
    memcpy(&saved, offset, sizeof(saved));
    uint32_t bad = saved + BitSet::BLOCK_ALIGN;
    memcpy(offset, &bad, sizeof(bad));
    auto restoreOffset = scope_guard([&] { memcpy(offset, &saved, sizeof(saved)); });
    auto segment = core.segmentName();
    auto file = Postings::getIndexFileName(segment, prepared.presence.bitmap.location.filenum());
    auto message = std::format("Segment '{}', file '{}', field 'text' is corrupt or in an unsupported format: invalid docs bitmap",
                               segment, file);
    reject("text", [](auto&) {}, message,
           [&](const DecodedFieldRecord& field) { DocsReader reader(postings, field); });
  } else {
    EncodedFieldRecord numeric(*postings.fieldInfo("number"), NUM_DOCS);
    for (auto ordinal : {-1, postings.numFields()}) {
      auto field = FieldRecordCodec::decode(numeric.bytes, NUM_DOCS, files, ordinal);
      EXPECT_THROW(postings.openNumericLeafMeta(*field), std::out_of_range);
    }
  }
  auto& textInfo = *postings.fieldInfo("text");
  ASSERT_FALSE(terms.terms.location.isNull());
  const auto& t = terms.terms;
  ASSERT_EQ(1u, t.runs.size());
  const auto& run = t.runs.front();
  ASSERT_TRUE(run.docs && run.positions);
  auto offsets = textInfo.termBlockOffsets();
  auto dictionaryBlocks = t.dictionaryBlocks();
  auto trie = textInfo.termTrie();
  checkSlices(core, {dictionaryBlocks, offsets, trie, run.docs, run.positions});
  EXPECT_LE(offsets.location.offset() - dictionaryBlocks.end().offset(), field_parts::Terms::OFFSETS_ALIGN - 1);
  EXPECT_EQ(sizeof(field_parts::Terms::BlockOffset), offsets.bytes);
  EXPECT_LT(t.trieRootOffset, trie.bytes);
  EXPECT_EQ(t, *textInfo.terms());
  {
    TermsEnum en(inverter.pool, postings, textInfo);
    ASSERT_TRUE(en.seek("word"));
    const auto state = en.postingsState();
    ASSERT_LT(state.docsStart, state.docsEnd);
    EXPECT_EQ(run.docs.bytes, (uint64_t)state.docIS.size());
    EXPECT_EQ(run.positions.bytes, (uint64_t)state.posIS.size());
    DocsPosEnum docs(en);
    PosEnum pos(docs);
    ASSERT_EQ(0, docs.nextDoc());
    EXPECT_EQ(2, docs.termFreq());
    pos.startPositions();
    EXPECT_EQ(0, pos.nextPosition());
    EXPECT_EQ(2, pos.nextPosition());
  }

  auto& numInfo = *postings.fieldInfo("number");
  TermsEnum emptyTerms(inverter.pool, postings, numInfo);
  emptyTerms.seekOrd(0);
  EXPECT_TRUE(emptyTerms.docFreqBlock(0).docFreqs.empty());
  EXPECT_FALSE(emptyTerms.nextTerm());
  checkSlices(core, {n.blocks(), n.index()});
  EXPECT_EQ(n.index().end().offset() - n.blocks().location.offset(), numericCheckoutBytes);
  ASSERT_TRUE(numInfo.numberColumn());
  EXPECT_EQ(*numInfo.numberColumn(), number.column);
  auto blocks = core.slice(n.blocks());
  auto indexBytes = core.slice(n.index());
  EXPECT_FALSE(NumColumnFormat::LeafMeta::decode(indexBytes.first(indexBytes.size() - 1), NUM_DOCS));
  EXPECT_FALSE(numInfo.ordColumn());
  EXPECT_FALSE(numInfo.bytesColumn());
  IntColReader currentNumbers(postings, numInfo);
  IntColReader::Iterator numberDocs(currentNumbers);
  IntColReader numberAtSlice(blocks.data(), indexBytes, NUM_DOCS);
  IntColReader::Iterator slicedNumbers(numberAtSlice);
  for (int32_t i = 0; i < NUM_DOCS; ++i) {
    ASSERT_EQ(i, slicedNumbers.next());
    ASSERT_EQ(i, numberDocs.next());
    EXPECT_EQ(numericValue(i), slicedNumbers.value());
    EXPECT_EQ(numberDocs.value(), slicedNumbers.value());
  }

  auto& ordInfo = *postings.fieldInfo("ord");
  const auto& o = ord.column;
  EXPECT_EQ(alternate ? field_parts::OrdMode::PREDICTED : field_parts::OrdMode::DIRECT, o.mode);
  EXPECT_EQ(alternate ? field_parts::OrdIndexing::RANK : field_parts::OrdIndexing::DOC_ID, o.indexing);
  auto metadata = ordInfo.ordBlockMetadata(NUM_DOCS);
  if (alternate) {
    ASSERT_TRUE(metadata && ord.endValueRanks);
    checkSlices(core, {o.values(), metadata});
    EXPECT_EQ(sizeof(OrdColumnFormat::PredictedBlockInfo), metadata.bytes);
    const std::array<int64_t, 4> ranks{2, 3, 4, 5};
    checkMono(core, ord.endValueRanks, ranks);
  } else {
    checkSlices(core, {o.values()});
    EXPECT_EQ(LinearPack::packedByteSize(NUM_DOCS, 2), o.values().bytes);
    EXPECT_FALSE(metadata || ord.endValueRanks);
  }
  ASSERT_TRUE(ordInfo.ordColumn());
  EXPECT_EQ(*ordInfo.ordColumn(), ord.column);
  EXPECT_FALSE(ordInfo.numberColumn());
  EXPECT_FALSE(ordInfo.bytesColumn());
  OrdColReader currentOrds(postings, ordInfo);
  for (int32_t doc = 0; doc < NUM_DOCS; ++doc) {
    EXPECT_EQ(doc % 2 + 1, currentOrds.firstOrdAt(doc));
  }

  auto& bytesInfo = *postings.fieldInfo("bytes");
  const auto& b = bytes.column;
  ASSERT_TRUE(bytes.presence.bitmap);
  checkSlices(core, {b.values});
  checkSlices(core, {bytes.presence.bitmap});
  EXPECT_EQ(bytes.presence.bitmap, bytesInfo.docsBitmap());
  DocsReader bytesDocs(postings, bytesInfo);
  ASSERT_TRUE(bytesDocs.bitset());
  EXPECT_EQ(openSlice(core, bytes.presence.bitmap).ptr(), bytesDocs.bitset()->start);
  EXPECT_EQ(1, bytesDocs.rankOf(2));
  EXPECT_EQ(-1, bytesDocs.rankOf(1));
  ASSERT_TRUE(bytesInfo.bytesColumn());
  EXPECT_EQ(*bytesInfo.bytesColumn(), bytes.column);
  auto last = alternate ? std::string_view("blue") : std::string_view("tan");
  EXPECT_EQ(3 + last.size(), b.values.bytes);
  EXPECT_EQ(last, StrColReader(postings, bytesInfo).valueAt(1));
  if (alternate) {
    const std::array<int64_t, 2> offsets{3, 7};
    checkMono(core, b.endOffsets, offsets);
  } else {
    EXPECT_EQ(3, bytesInfo.bytesStride());
  }
  const auto& v = vectors.column;
  ASSERT_TRUE(vectors.owners);
  checkSlices(core, {v.values});
  const std::array<int64_t, 1> owners{0};
  checkMono(core, vectors.owners, owners);
  EXPECT_EQ(sizeof(vec), v.values.bytes);
  auto& vectorInfo = *postings.fieldInfo("vec");
  const auto* vector = vectorInfo.bytesColumn();
  ASSERT_TRUE(vector);
  EXPECT_EQ(*vector, vectors.column);
  EXPECT_FALSE(vectorInfo.numberColumn());
  EXPECT_FALSE(vectorInfo.ordColumn());
  ASSERT_GE(vectorInfo.bytesStride(), 0);
  EXPECT_EQ(sizeof(vec), vectorInfo.bytesStride());
  EXPECT_EQ(std::string_view((const char*)vec.data(), sizeof(vec)),
            StrColReader(postings, vectorInfo).valueAt(0));

  ASSERT_TRUE(norms);
  checkSlices(core, {textInfo.normsValues(NUM_DOCS)});
  const auto& normInfo = textInfo;
  EXPECT_EQ(*normInfo.norms(), *norms);
  EXPECT_EQ((uint64_t)normCount, textInfo.normsValues(NUM_DOCS).bytes);
  EXPECT_EQ(alternate ? field_parts::NormsMode::SPARSE : field_parts::NormsMode::FLAT, norms->mode);
  EXPECT_EQ(2u, NormsReader(postings, normInfo).value(0));
  EXPECT_EQ(alternate, prepared.presence.bitmap.has_value());
  if (prepared.presence.bitmap) {
    checkSlices(core, {prepared.presence.bitmap});
    EXPECT_EQ(prepared.presence.bitmap, normInfo.docsBitmap());
  }
  if (alternate) checkPoints<BKDReader>(core, numInfo, points);
  else checkPoints<PointsReader>(core, numInfo, points);

  const auto& s = *stored;
  EXPECT_EQ(s.chunks().end(), s.dictionary() ? s.dictionary().location : s.names().location);
  if (s.dictionary()) {
    EXPECT_EQ(s.dictionary().end(), s.names().location);
    checkSlices(core, {s.chunks(), s.dictionary(), s.names()});
    EXPECT_EQ(dict, std::string_view(openSlice(core, s.dictionary()).ptr(), s.dictionary().bytes));
  } else {
    checkSlices(core, {s.chunks(), s.names()});
  }
  EXPECT_EQ(alternate, s.dictionary().has_value());
  auto names = openSlice(core, s.names());
  EXPECT_EQ(1u, names.checkedVint());
  EXPECT_EQ("original", std::string_view(names.checkedPackedTerm()));
  EXPECT_EQ(0u, names.left());
  const std::array<int64_t, 1> firstDocs{0};
  const std::array<int64_t, 2> chunkOffsets{0, (int64_t)s.chunksBytes};
  checkMono(core, s.firstDocs, firstDocs);
  checkMono(core, s.chunkOffsets, chunkOffsets);
  auto& storedInfo = *postings.fieldInfo("_stored_");
  auto rejectLast = [&](Slice slice, auto&& open, std::string_view reason) {
    auto bytes = core.slice(slice);
    auto* last = const_cast<char*>(bytes.data()) + bytes.size() - sizeof(NumBlockInfo);
    auto saved = loadUnaligned<NumBlockInfo>(last);
    NumBlockInfo wrong;  // Represents a constant zero block.
    std::memcpy(last, &wrong, sizeof(wrong));
    auto restore = scope_guard([&] { std::memcpy(last, &saved, sizeof(saved)); });
    try {
      open();
      ADD_FAILURE() << "Expected corruption: " << reason;
    } catch (const std::runtime_error& e) {
      EXPECT_TRUE(std::string_view(e.what()).contains(reason)) << e.what();
    }
  };
  if (alternate) {
    rejectLast(b.endOffsets, [&] { StrColReader reader(postings, bytesInfo); },
               "last end offset differs");
    rejectLast(ord.endValueRanks, [&] { OrdColReader reader(postings, ordInfo); },
               "last end-value rank differs");
  }
  rejectLast(s.chunkOffsets, [&] { StoredFieldsReader reader(postings, storedInfo); },
             "last chunk offset differs");
  EXPECT_EQ(*storedInfo.stored(), s);
  StoredFieldsReader reader(postings, storedInfo);
  EXPECT_EQ(s.numChunks, reader.numChunks());
  EXPECT_EQ(s.maxChunkBytes, reader.maxChunkBytes());
  EXPECT_EQ(s.numDirtyChunks, reader.numDirtyChunks());
  EXPECT_EQ(s.codec, reader.format().codec);
  EXPECT_EQ(s.chunkTarget, reader.format().chunkTarget);
  EXPECT_EQ(s.maxDocsPerChunk, reader.format().maxDocsPerChunk);
  EXPECT_EQ((int64_t)s.chunksBytes, reader.chunkRegionBytes());
  int members = 0;
  reader.readDoc(0, [&](std::string_view name, std::span<const std::string_view> values) {
    ++members;
    EXPECT_EQ("original", name);
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ("stored value", values[0]);
  });
  EXPECT_EQ(1, members);
}

INSTANTIATE_TEST_SUITE_P(Formats, FieldPartsTest, ::testing::Bool());

} // namespace
} // namespace luxir::test
