#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/codec/StreamVByte.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/schema/Schema.h"
#include "luxir/store/FSDirectory.h"

using namespace luxir;

namespace {

class TempDirectory {
  std::filesystem::path path_;

public:
  TempDirectory() {
    std::string pattern = "/tmp/luxir-compound-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) throw std::runtime_error("mkdtemp failed");
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }
};

std::shared_ptr<Schema> compoundSchema() {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
      "whitespace");
  schema->fieldTypeMap["tag"] = std::make_shared<StrFieldType>(
      "tag", FieldType::COLUMN_STORED);
  schema->fieldTypeMap["score"] = std::make_shared<IntFieldType>(
      "score", FieldType::COLUMN_STORED | FieldType::INDEX_RANGE);
  return schema;
}

SegFieldInfo fieldInfo(PostingsReader& reader, std::string_view field) {
  FieldReader fields(reader);
  EXPECT_TRUE(fields.seek(field));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

std::vector<std::string> baseSegmentFiles(Directory& directory, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  std::string prefix = Postings::getIndexFileNamePrefix(segId);
  std::vector<std::string> matches;
  for (const auto& file : files) {
    if (file.name.starts_with(prefix + "_")
        && !file.name.starts_with(prefix + "__")) {
      matches.push_back(file.name);
    }
  }
  return matches;
}

std::vector<std::pair<std::string, std::string>> storedValues(
    StoredFieldsReader& reader, int32_t doc) {
  std::vector<std::pair<std::string, std::string>> values;
  reader.readDoc(doc, [&](std::string_view field,
                          std::span<const std::string_view> fieldValues) {
    for (std::string_view value : fieldValues) {
      values.emplace_back(field, value);
    }
  });
  return values;
}

void verifyTinySegment(Directory& directory) {
  auto schema = compoundSchema();
  IndexWriter writer(directory, [schema] { return schema; });
  Inverter& inverter = writer.obtainInverter();

  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("alpha beta"));
  inverter.getIndexHandler("tag").index(inverter, std::string_view("x"));
  inverter.getIndexHandler("score").index(inverter, (int64_t) 7);
  inverter.finishDoc();

  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter, std::string_view("beta"));
  inverter.getIndexHandler("score").index(inverter, (int64_t) -2);
  inverter.finishDoc();

  inverter.startDoc();
  inverter.getIndexHandler("tag").index(inverter, std::string_view("y"));
  inverter.finishDoc();

  writer.releaseInverter(inverter, true);
  writer.commit();
  auto reader = writer.getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  Segment& segment = reader->segments()[0];
  ASSERT_EQ(1u, baseSegmentFiles(directory, segment.segInfo.seg_id).size());

  PostingsReader& postings = segment.postingsReader();
  SegFieldInfo bodyInfo = fieldInfo(postings, "body");
  MemPool pool;
  TermsEnum terms(pool, postings, bodyInfo);
  ASSERT_TRUE(terms.seek("beta"));
  DocsPosEnum docs(terms);
  PosEnum positions(docs);
  ASSERT_EQ(0, docs.nextDoc());
  EXPECT_EQ(1, docs.termFreq());
  positions.startPositions();
  EXPECT_EQ(1, positions.nextPosition());
  ASSERT_EQ(1, docs.nextDoc());
  EXPECT_EQ(1, docs.termFreq());
  positions.startPositions();
  EXPECT_EQ(0, positions.nextPosition());
  EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());

  NormsReader norms(postings, bodyInfo);
  EXPECT_EQ(2, (int32_t) norms.value(0));
  EXPECT_EQ(1, (int32_t) norms.value(1));

  StrColReader tags(postings, fieldInfo(postings, "tag"));
  StrColReader::Iterator tagIter(tags);
  ASSERT_EQ(0, tagIter.next());
  EXPECT_EQ("x", tagIter.value());
  ASSERT_EQ(2, tagIter.next());
  EXPECT_EQ("y", tagIter.value());
  EXPECT_EQ(StrColReader::Iterator::ENDDOC, tagIter.next());

  SegFieldInfo scoreInfo = fieldInfo(postings, "score");
  IntColReader scores(postings, scoreInfo);
  IntColReader::Iterator scoreIter(scores);
  ASSERT_EQ(0, scoreIter.next());
  EXPECT_EQ(7, scoreIter.value());
  ASSERT_EQ(1, scoreIter.next());
  EXPECT_EQ(-2, scoreIter.value());
  EXPECT_EQ(IntColReader::ENDDOC, scoreIter.next());
  PointsReader points(postings, scoreInfo);
  std::vector<PointsReader::Point> expectedPoints = {{-2, 1}, {7, 0}};
  EXPECT_EQ(expectedPoints, points.readAll());

  auto stored = StoredFieldsReader::open(postings);
  ASSERT_NE(nullptr, stored);
  EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                {"body", "alpha beta"}}),
            storedValues(*stored, 0));
  EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                {"body", "beta"}}),
            storedValues(*stored, 1));
  EXPECT_TRUE(storedValues(*stored, 2).empty());
}

void addMergeSource(IndexWriter& writer, int32_t source) {
  Inverter& inverter = writer.obtainInverter();
  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("keep common"));
  inverter.getIndexHandler("score").index(inverter, (int64_t) source);
  inverter.finishDoc();
  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("gone common"));
  inverter.getIndexHandler("score").index(inverter,
                                            (int64_t) source + 100);
  inverter.finishDoc();
  inverter.deleteDoc(1);
  writer.releaseInverter(inverter, true);
  writer.commit();
}

} // namespace

TEST(CompoundFileTest, TinyFlushCollapsesOnRAMDirectory) {
  RAMDir directory;
  verifyTinySegment(directory);
}

TEST(CompoundFileTest, TinyFlushCollapsesOnFSDirectory) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  verifyTinySegment(directory);
}

TEST(CompoundFileTest, RelocationPreservesSparseFilenumAndTailSlack) {
  constexpr uint32_t VALUE_COUNT = 33;
  RAMDir directory;
  PostingsWriter writer(directory, 17, 1);
  auto streams = writer.getOutputStreams<3>();
  ASSERT_EQ(0, streams[0]->streamNumber);
  ASSERT_EQ(1, streams[1]->streamNumber);
  ASSERT_EQ(2, streams[2]->streamNumber);

  std::array<uint32_t, VALUE_COUNT> values;
  for (uint32_t i = 0; i < VALUE_COUNT; i++) values[i] = i * i + 3;
  uint32_t keyBytes = svbKeyBytes(VALUE_COUNT);
  std::vector<uint8_t> encoded(keyBytes + VALUE_COUNT * sizeof(uint32_t));
  uint8_t* encodedEnd = svb_encode_scalar_d1_init(
      values.data(), encoded.data(), encoded.data() + keyBytes, VALUE_COUNT, 0);
  encoded.resize((size_t) (encodedEnd - encoded.data()));

  auto& moved = writer.addField("moved");
  moved.type = FieldType::BIN;
  moved.flags = FieldType::COLUMN_STORED;
  moved.docsWithField = 1;
  moved.numValues = 1;
  moved.columnLoc = streams[1]->slocation();
  moved.columnMetaOff = (int64_t) encoded.size();
  streams[1]->write(encoded.data(), encoded.size());

  auto& survivor = writer.addField("survivor");
  survivor.type = FieldType::BIN;
  survivor.flags = FieldType::COLUMN_STORED;
  survivor.docsWithField = 1;
  survivor.numValues = 1;
  survivor.columnLoc = streams[2]->slocation();
  survivor.columnMetaOff = 1;
  streams[2]->write((char) 0x5a);
  streams[2]->flush();

  for (auto& stream : streams) stream.reset();
  writer.finish();

  std::string seg = Postings::getSortableString(17);
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  ASSERT_EQ(2u, files.size());
  EXPECT_EQ(nullptr, directory.openFile(Postings::getIndexFileName(seg, 1)).get());
  EXPECT_NE(nullptr, directory.openFile(Postings::getIndexFileName(seg, 2)).get());

  PostingsReader reader(directory, 17);
  SegFieldInfo movedInfo = fieldInfo(reader, "moved");
  SegFieldInfo survivorInfo = fieldInfo(reader, "survivor");
  EXPECT_EQ(0u, movedInfo.columnLoc.filenum());
  EXPECT_EQ(2u, survivorInfo.columnLoc.filenum());
  EXPECT_EQ((uint64_t) 0, movedInfo.columnLoc.offset() % 8);

  InputStream movedInput = reader.getInputStreamSeek(movedInfo.columnLoc);
  ASSERT_GE(movedInput.left(),
            (int64_t) encoded.size() + (int64_t) SVB_OVERREAD_PAD);
  std::array<uint32_t, VALUE_COUNT> decoded;
  uint8_t* keys = (uint8_t*) movedInput.ptr();
  svb_decode_avx_d1_init(decoded.data(), keys, keys + keyBytes,
                         VALUE_COUNT, 0);
  EXPECT_EQ(values, decoded);

  InputStream survivorInput = reader.getInputStreamSeek(survivorInfo.columnLoc);
  EXPECT_EQ((char) 0x5a, survivorInput.readByte());
  EXPECT_EQ((uint64_t) files[0].size + files[1].size, reader.sizeInBytes());
}

TEST(CompoundFileTest, MergeReadsCollapsedSourcesWithDeletes) {
  RAMDir directory;
  auto schema = compoundSchema();
  IndexWriter writer(directory, [schema] { return schema; });
  writer.mergePolicy->setMergeFactor(1000);
  for (int32_t source = 0; source < 3; source++) addMergeSource(writer, source);

  auto sources = writer.getIndexReader();
  ASSERT_EQ(3u, sources->segments().size());
  for (Segment& segment : sources->segments()) {
    EXPECT_EQ(1u, baseSegmentFiles(directory, segment.segInfo.seg_id).size());
    EXPECT_EQ(1, segment.numDeletes());
  }

  writer.mergeSegments();
  writer.commit();
  auto merged = writer.getIndexReader();
  ASSERT_EQ(1u, merged->segments().size());
  Segment& segment = merged->segments()[0];
  EXPECT_EQ(3, segment.maxDoc());
  EXPECT_EQ(1u, baseSegmentFiles(directory, segment.segInfo.seg_id).size());

  PostingsReader& postings = segment.postingsReader();
  SegFieldInfo bodyInfo = fieldInfo(postings, "body");
  MemPool pool;
  TermsEnum terms(pool, postings, bodyInfo);
  ASSERT_TRUE(terms.seek("keep"));
  DocsPosEnum docs(terms);
  EXPECT_EQ(0, docs.nextDoc());
  EXPECT_EQ(1, docs.nextDoc());
  EXPECT_EQ(2, docs.nextDoc());
  EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());
  EXPECT_FALSE(terms.seek("gone"));

  IntColReader scores(postings, fieldInfo(postings, "score"));
  IntColReader::Iterator scoreIter(scores);
  for (int32_t doc = 0; doc < 3; doc++) {
    ASSERT_EQ(doc, scoreIter.next());
    EXPECT_EQ(doc, scoreIter.value());
  }

  auto stored = StoredFieldsReader::open(postings);
  ASSERT_NE(nullptr, stored);
  for (int32_t doc = 0; doc < 3; doc++) {
    EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                  {"body", "keep common"}}),
              storedValues(*stored, doc));
  }
}
