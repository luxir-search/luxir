// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldRecordCodec.h"

#include "CatalogIO.h"
#include "IdFilter.h"
#include "Space.h"
#include "luxir/index/IndexFieldInfo.h"

#include <array>
#include <limits>
#include <new>
#include <type_traits>

#include <boost/container/small_vector.hpp>

#include "luxir/codec/OrdColumnFormat.h"
#include "luxir/codec/StreamVByte.h"
#include "luxir/codec/VectorCodec.h"
#include "luxir/codec/NumColumnFormat.h"
#include "luxir/util/screaming.h"

namespace luxir {

namespace {

using namespace field_parts;
using namespace catalog_io;

constexpr char RECORD_ERROR[] = "invalid field record";
constexpr char RECORD_OUTPUT_ERROR[] = "invalid field record output";
constexpr int NUM_PARTS = (int)FieldPart::COUNT;
// Only the parts defined by this format may appear in the mask.
constexpr uint16_t PART_MASK = (1u << NUM_PARTS) - 1;
// The state byte carries both segment flags and these record layout bits.
constexpr uint8_t HAS_BOUNDS = 1u << 6;
constexpr uint8_t EXPLICIT_NUM_VALUES = 1u << 7;
constexpr uint8_t RECORD_STATE_BITS = HAS_BOUNDS | EXPLICIT_NUM_VALUES;
static_assert((FieldSegmentFlags::ALL & RECORD_STATE_BITS) == 0);
constexpr uint8_t STATE_MASK = FieldSegmentFlags::ALL | RECORD_STATE_BITS;

using Lower = DictionaryBounds::Lower;
using Upper = DictionaryBounds::Upper;

// Two endpoint modes share the dictionary bounds option byte.
struct DictionaryOptions {
  static constexpr int UPPER_SHIFT = std::bit_width((uint8_t)Lower::LAST);
  static constexpr uint8_t LOWER_MASK = (1u << UPPER_SHIFT) - 1;
  static constexpr uint8_t UPPER_MASK = (1u << std::bit_width((uint8_t)Upper::LAST)) - 1;
  static constexpr uint8_t ALL = LOWER_MASK | (UPPER_MASK << UPPER_SHIFT);
};

// Each run has a bit per optional posting stream.
struct StreamOptions {
  static constexpr uint8_t DOCS = 1u << 0;
  static constexpr uint8_t POSITIONS = 1u << 1;
  static constexpr uint8_t ALL = DOCS | POSITIONS;
  static constexpr int BITS = std::bit_width(ALL);
  static constexpr int RUNS_PER_BYTE = CHAR_BIT / BITS;
  // Tags beyond the singleton masks encode the count of two or more runs.
  static constexpr int RUN_TAG_BASE = ALL + 1 - 2;
};

// Options for an ordinal column.
struct OrdOptions {
  static constexpr uint8_t PREDICTED = 1u << 0;
  static constexpr uint8_t DOC_INDEXED = 1u << 1;
};

// A bytes column with variable values carries the end offsets slice.
constexpr uint8_t HAS_END_OFFSETS = 1u << 0;
// A vector column in an encoding other than f32 carries the encoding byte.
constexpr uint8_t HAS_VECTOR_ENCODING = 1u << 1;
// Sparse norm bytes are indexed by docs-with-field rank.
constexpr uint8_t SPARSE_NORMS = 1u << 0;

template <class Mode>
Mode readMode(Reader& in, uint8_t value, const char* reason) {
  if (value > (uint8_t)Mode::LAST) in.invalid(reason);
  return (Mode)value;
}

void writeBounds(Writer& out, ScalarBounds bounds) {
  out.writeVarUInt(zigzagEncode(bounds.min));
  out.writeVarUInt((uint64_t)bounds.max - (uint64_t)bounds.min);
}

void writeBounds(Writer& out, const DictionaryBounds& bounds) {
  out.writeByte((uint8_t)bounds.lowerMode
      | ((uint8_t)bounds.upperMode << DictionaryOptions::UPPER_SHIFT));
  out.writePackedTerm(bounds.lower);
  if (bounds.upperMode != Upper::UNBOUNDED) {
    out.writePackedTerm(bounds.upper);
  }
}

void writeBounds(Writer& out, const GeoBounds& bounds) {
  out.writeLittleEndian(bounds.lat.min);
  out.writeLittleEndian(bounds.lat.max);
  out.writeLittleEndian(bounds.lon.min);
  out.writeLittleEndian(bounds.lon.max);
}

void writeBounds(Writer& out, const IntervalBounds& bounds) {
  writeBounds(out, bounds.low);
  writeBounds(out, bounds.high);
}

ScalarBounds readScalarBounds(Reader& in) {
  auto min = zigzagDecode(in.readVarUInt());
  auto delta = in.readVarUInt((uint64_t)INT64_MAX - (uint64_t)min);
  return {min, (int64_t)((uint64_t)min + delta)};
}

Bounds readBounds(Reader& in, BoundsKind kind) {
  switch (kind) {
    case BoundsKind::NONE: return {};
    case BoundsKind::SCALAR: return readScalarBounds(in);
    case BoundsKind::DICTIONARY: {
      auto options = in.readOptions(DictionaryOptions::ALL);
      auto lowerMode = readMode<Lower>(in, options & DictionaryOptions::LOWER_MASK,
                                      "lower dictionary mode");
      auto upperMode = readMode<Upper>(in, (options >> DictionaryOptions::UPPER_SHIFT) & DictionaryOptions::UPPER_MASK,
                                      "upper dictionary mode");
      auto lower = in.readPackedTerm("lower dictionary endpoint outside record");
      auto upper = upperMode == Upper::UNBOUNDED ? PackedTerm(nullptr)
          : in.readPackedTerm("upper dictionary endpoint outside record");
      return DictionaryBounds{lower, upper, lowerMode, upperMode};
    }
    case BoundsKind::GEO: return GeoBounds{{in.readLittleEndian32(), in.readLittleEndian32()},
                                          {in.readLittleEndian32(), in.readLittleEndian32()}};
    case BoundsKind::INTERVAL: return IntervalBounds{readScalarBounds(in), readScalarBounds(in)};
  }
  std::unreachable();
}

ColumnKind columnKind(const FieldRecordHeader& header) { return field_parts::columnKind(header.type, header.flags); }

// Checks the bounds kind, presence and endpoints against the header.
void checkBounds(const Errors& errors, const FieldRecordHeader& header) {
  auto type = header.type;
  const auto& bounds = header.bounds;
  auto kind = field_parts::boundsKind(type, header.flags);
  if (std::holds_alternative<DictionaryBounds>(bounds) && kind != BoundsKind::DICTIONARY) errors.invalid("dictionary bounds for field type");
  if (std::holds_alternative<GeoBounds>(bounds) && kind != BoundsKind::GEO) errors.invalid("geo bounds for field type");
  if (std::holds_alternative<IntervalBounds>(bounds) && kind != BoundsKind::INTERVAL) errors.invalid("interval bounds for field type");
  const auto* dictionary = std::get_if<DictionaryBounds>(&bounds);
  if (type == FieldType::ID && header.parts.has(FieldPart::TERMS)
      && (!dictionary || dictionary->lowerMode != Lower::EXACT || dictionary->upperMode != Upper::INCLUSIVE)) {
    errors.invalid("ID dictionary requires exact bounds");
  }
  auto* scalar = std::get_if<ScalarBounds>(&bounds);
  bool hasValues = header.numValues != 0;
  if (hasValues && kind == BoundsKind::SCALAR && !scalar) errors.invalid("numeric values require scalar bounds");
  if (scalar && (kind != BoundsKind::SCALAR || !hasValues)) errors.invalid("scalar bounds without numeric values");
  if (hasValues && kind == BoundsKind::GEO && !std::holds_alternative<GeoBounds>(bounds)) {
    errors.invalid("geo values require geo bounds");
  }
  if (hasValues && kind == BoundsKind::INTERVAL && !std::holds_alternative<IntervalBounds>(bounds)) {
    errors.invalid("interval values require interval bounds");
  }
  if (scalar && scalar->min > scalar->max) errors.invalid("inverted scalar bounds");
  if (dictionary) {
    if (!header.parts.has(FieldPart::TERMS)) errors.invalid("dictionary bounds without terms");
    if (dictionary->lowerMode > Lower::LAST || dictionary->upperMode > Upper::LAST) errors.invalid("dictionary bounds mode");
    auto cap = FieldType::dictionaryBoundsCap(type);
    if (dictionary->lower.isNull()) errors.invalid("missing lower dictionary endpoint");
    if (dictionary->lower.size() > cap) errors.invalid("lower dictionary endpoint too long");
    if (dictionary->lowerMode == Lower::ROUNDED && dictionary->lower.size() != cap) errors.invalid("rounded lower endpoint length");
    if (dictionary->upperMode == Upper::UNBOUNDED) {
      if (!dictionary->upper.isNull()) errors.invalid("unbounded dictionary has upper endpoint");
    } else {
      if (dictionary->upper.isNull()) errors.invalid("missing upper dictionary endpoint");
      if (dictionary->upper.size() > cap) errors.invalid("upper dictionary endpoint too long");
      if (dictionary->upper < dictionary->lower
          || (dictionary->upper == dictionary->lower && dictionary->upperMode == Upper::EXCLUSIVE)) {
        errors.invalid("inverted dictionary bounds");
      }
    }
  }
  if (auto* geo = std::get_if<GeoBounds>(&bounds)) {
    if (!hasValues) errors.invalid("geo bounds without values");
    if (geo->lat.min > geo->lat.max || geo->lon.min > geo->lon.max) errors.invalid("inverted geo bounds");
  }
  if (auto* interval = std::get_if<IntervalBounds>(&bounds)) {
    if (!hasValues) errors.invalid("interval bounds without values");
    if (interval->low.min > interval->low.max || interval->high.min > interval->high.max) errors.invalid("inverted endpoint bounds");
    if (interval->low.min > interval->high.min || interval->low.max > interval->high.max) errors.invalid("inverted interval bounds");
  }
}

// Checks whether the field and its flags permit the value count.
void checkColumnCount(const Errors& errors, const FieldRecordHeader& header) {
  auto values = header.numValues;
  auto docs = header.docsWithField;
  if (!header.parts.has(FieldPart::COLUMN)) {
    if (values) errors.invalid("values without a column");
    return;
  }
  if (columnKind(header) == ColumnKind::NONE) errors.invalid("column not supported by field type");
  if (values < docs) errors.invalid("fewer values than docs with field");
  if (!(header.flags & FieldType::MULTI_VALUED) && values != docs) errors.invalid("single-value column count");
  if (header.type == FieldType::INTERVAL && values % 2) errors.invalid("unpaired interval values");
}

// Checks the header without touching any part.
void checkHeader(const Errors& errors, const FieldRecordHeader& header, int32_t maxDoc) {
  auto type = header.type;
  auto flags = header.flags;
  auto parts = header.parts;
  auto docs = header.docsWithField;
  auto values = header.numValues;
  if (parts.mask() & ~PART_MASK) errors.invalid("unknown field part bits");
  if (!validSpaceDocCount(maxDoc)) errors.invalid("invalid row space doc count");
  if (header.name.isNull()) errors.invalid("missing field name");
  if ((unsigned)type > FieldType::LAST_TYPE || FieldType::storage(type) == FieldType::Storage::NONE) {
    errors.invalid("nonphysical field type");
  }
  if (auto* reason = FieldType::typeParametersError(type, header.typeParameters)) errors.invalid(reason);
  if (flags & FieldType::SCHEMA_ROLES) errors.invalid("schema role in field flags");
  if (flags & ~FieldType::KNOWN_FLAGS) errors.invalid("unknown flags");
  auto level = flags & FieldType::INDEX_DOCS_FREQS_POSITIONS;
  if (level && level != FieldType::INDEX_DOCS && level != FieldType::INDEX_DOCS_FREQS
      && level != FieldType::INDEX_DOCS_FREQS_POSITIONS) errors.invalid("invalid postings level");
  if ((flags & FieldType::MULTI_ARRAY) && !(flags & FieldType::MULTI_VALUED)) errors.invalid("array must be multivalued");
  if (docs < 0 || docs > maxDoc) errors.invalid("docs with field exceeds space doc count");
  if (docs == 0) errors.invalid("field record has no docs");
  if (docs < maxDoc && !parts.has(FieldPart::DOCS)) errors.invalid("sparse field without a docs bitmap");
  if (docs == maxDoc && parts.has(FieldPart::DOCS)) errors.invalid("dense field has a docs bitmap");
  checkColumnCount(errors, header);

  if (parts.has(FieldPart::VALUE_RANKS)) {
    if (!parts.has(FieldPart::COLUMN)) errors.invalid("end-value ranks without column");
    if (!(flags & FieldType::MULTI_VALUED)) errors.invalid("end-value ranks on single-value field");
  } else if (parts.has(FieldPart::COLUMN) && docs != values) {
    errors.invalid("multiple values without end-value ranks");
  }
  if (parts.has(FieldPart::OWNERS) && (!parts.has(FieldPart::COLUMN)
      || type != FieldType::VECTOR || !(flags & FieldType::MULTI_VALUED))) errors.invalid("reverse owners on non-multivalue vector");
  if (parts.has(FieldPart::FILTER) && (type != FieldType::ID || !parts.has(FieldPart::TERMS))) {
    errors.invalid("id filter without an id dictionary");
  }

  auto storage = FieldType::storage(type);
  bool lexical = storage == FieldType::Storage::TERMS || storage == FieldType::Storage::TEXT;
  if (header.segFlags & ~FieldSegmentFlags::ALL) errors.invalid("unknown segment flags");
  if ((header.segFlags & FieldSegmentFlags::MAY_HAVE_FITTED) && !lexical) errors.invalid("fitted flag on nonlexical field");
  if (flags & FieldType::INDEX_DOCS) {
    if (!lexical && (!FieldType::isNumericColumn(type) || level != FieldType::INDEX_DOCS)) {
      errors.invalid("postings level for field type");
    }
    const auto& [terms, df, ttf] = header.indexedTotals;
    if (terms < 0 || df < terms || ttf < df) errors.invalid("indexed totals out of order");
    if (!terms && ttf) errors.invalid("postings without terms");
    if (!FieldType::hasFreqs(flags) && df != ttf) errors.invalid("term frequencies without frequency indexing");
  } else if (header.indexedTotals != IndexedTotals{}) {
    errors.invalid("totals on unindexed field");
  }
  if (parts.has(FieldPart::COLUMN) && columnKind(header) == ColumnKind::ORD
      && (!parts.has(FieldPart::TERMS) || header.indexedTotals.numTerms <= 0)) {
    errors.invalid("ordinal column requires a nonempty terms dictionary");
  }
  if (parts.has(FieldPart::TERMS) && !header.indexedTotals.numTerms) errors.invalid("terms part has no terms");
  if (!parts.has(FieldPart::TERMS) && header.indexedTotals.numTerms) errors.invalid("indexed terms have no terms part");
  if (parts.has(FieldPart::NORMS)) {
    if (storage != FieldType::Storage::TEXT) errors.invalid("norms on nontext field");
  }
  if (parts.has(FieldPart::POINTS)) {
    if (storage != FieldType::Storage::INT64) errors.invalid("points on incompatible field type");
    if (!(flags & FieldType::INDEX_RANGE)) errors.invalid("points without range indexing");
    if (!values) errors.invalid("points without values");
  }
  if (parts.has(FieldPart::STORED) != (storage == FieldType::Storage::BIN && bool(flags & FieldType::STORED))) {
    errors.invalid("stored resource presence");
  }
  if (parts.has(FieldPart::STORED)) {
    if (parts.size() != 1) errors.invalid("stored resource has other parts");
    if (docs != maxDoc) errors.invalid("stored resource doc count");
  }
  checkBounds(errors, header);
}

FieldRecordHeader readHeader(Reader& in) {
  FieldRecordHeader header;
  header.name = in.readPackedTerm("field name outside record");
  header.type = (FieldType::Type)in.readVarUInt(FieldType::LAST_TYPE);
  header.flags = (FieldType::flag_type)in.readVarUInt(UINT32_MAX);
  if (FieldType::hasTypeParameters(header.type)) header.typeParameters = (uint32_t)in.readVarUInt(UINT32_MAX);
  header.parts = FieldPartSet((uint16_t)in.readVarUInt(PART_MASK));
  auto state = in.readOptions(STATE_MASK);
  header.segFlags = state & FieldSegmentFlags::ALL;
  header.docsWithField = (int32_t)in.readVarUInt(INT32_MAX);
  if (state & EXPLICIT_NUM_VALUES) {
    if (!header.parts.has(FieldPart::COLUMN)) in.invalid("explicit values without column");
    header.numValues = (int64_t)in.readVarUInt(INT64_MAX);
    if (header.numValues == header.docsWithField) in.invalid("explicit value count equals docs with field");
  } else if (header.parts.has(FieldPart::COLUMN)) {
    header.numValues = header.docsWithField;
  }
  if (header.flags & FieldType::INDEX_DOCS) {
    auto terms = (int64_t)in.readVarUInt(INT64_MAX);
    auto df = terms + (int64_t)in.readVarUInt(INT64_MAX - terms);
    header.indexedTotals = {terms, df, df + (int64_t)in.readVarUInt(INT64_MAX - df)};
  }
  if (state & HAS_BOUNDS) {
    auto kind = boundsKind(header.type, header.flags);
    if (kind == BoundsKind::NONE) in.invalid("bounds for field type");
    header.bounds = readBounds(in, kind);
  }
  return header;
}

// Ranges checked after decoding the record. The common one-run case fits inline.
class Extents {
  boost::container::small_vector<Slice, 12> slices;  // Nonempty slices checked for overlap.
  std::optional<FileSizes> files;  // File inventory available to the reader.

public:
  explicit Extents(std::optional<FileSizes> files) : files(files) {}

  void check(const Errors& io, Slice slice) const {
    checkSlice(io, slice, files, "slice outside file");
  }

  void add(const Errors& io, Slice slice) {
    check(io, slice);
    if (slice.bytes) slices.push_back(slice);
  }

  void add(const Errors& io, Slice first, Slice last) {
    check(io, first);
    check(io, last);
    if (last.location.filenum() != first.location.filenum()
        || last.location.offset() < first.end().offset()) io.invalid("misplaced adjacent slices");
    add(io, {first.location, last.end().offset() - first.location.offset()});
  }

  void finish(const Errors& io) {
    std::ranges::sort(slices, {}, [](Slice slice) { return slice.location.raw(); });
    for (size_t i = 1; i < slices.size(); ++i) {
      if (slices[i].location.raw() < slices[i - 1].end().raw()) io.invalid("overlapping data slices");
    }
  }
};

// Shared validation for the descriptors written and the parts decoded.
class PartChecks {
  Extents extents;
  const FieldRecordHeader& header;  // Counts and type that describe the parts.
  int32_t maxDoc;  // Number of docs in the row space.

  void mono(const Errors& io, Slice slice, int64_t count, const char* reason) {
    extents.add(io, slice);
    if (!field_parts::monotonicBlocksBytes(slice, count)) io.invalid(reason);
  }

public:
  PartChecks(const FieldRecordHeader& header, int32_t maxDoc, std::optional<FileSizes> files)
      : extents(files), header(header), maxDoc(maxDoc) {}

  void dictionary(const Errors& io, const Terms& terms) {
    auto dictionary = terms.dictionaryBlocks();
    extents.check(io, dictionary);
    if (terms.trieRootOffset >= terms.trieBytes) io.invalid("trie root outside trie");
    if (termBlockOffsetsStart(dictionary.end().offset()) > seg_location::maxOffset()) {
      io.invalid("aligned term block offsets exceed the file offset limit");
    }
    auto offsets = termBlockOffsets(terms, header.indexedTotals.numTerms);
    extents.check(io, offsets);
    auto trie = termTrie(offsets, terms.trieBytes);
    if (offsets.bytes < SVB_OVERREAD_PAD && trie.bytes < SVB_OVERREAD_PAD - offsets.bytes) {
      io.invalid("insufficient term dictionary decoder padding");
    }
    extents.add(io, dictionary, trie);
  }

  void streams(const Errors& io, const TermRun& run) {
    if (run.docs) {
      if (!run.docs.bytes) io.invalid("empty docs stream");
      extents.add(io, run.docs);
    }
    if (run.positions) {
      if (!run.positions.bytes) io.invalid("empty positions stream");
      if (!FieldType::hasPositions(header.flags)) io.invalid("positions without position indexing");
      extents.add(io, run.positions);
    }
  }

  void part(const Errors& io, const NumberColumn& column) {
    extents.check(io, column.blocks());
    auto end = column.blocks().end().offset();
    if (NumColumnFormat::indexOffset(end) > seg_location::maxOffset()) io.invalid("numeric index alignment");
    extents.add(io, column.blocks(), column.index());
    auto numValues = header.numValues;
    if (!NumColumnFormat::validBlockBytes(column.blocksBytes, numValues)) io.invalid("numeric payload size");
    if (column.indexBytes < NumColumnFormat::LeafMeta::minimumIndexBytes(numValues)) io.invalid("truncated numeric index");
    if (!NumColumnFormat::LeafMeta::validIndexSize(column.indexBytes, numValues)) io.invalid("numeric sketch size");
  }

  void part(const Errors& io, const OrdColumn& column) {
    if (column.mode > OrdMode::LAST || column.indexing > OrdIndexing::LAST) io.invalid("ordinal mode");
    if (header.parts.has(FieldPart::VALUE_RANKS) && column.indexing == OrdIndexing::DOC_ID) {
      io.invalid("doc-indexed ordinals with end-value ranks");
    }
    auto width = OrdColumnFormat::width(header.indexedTotals.numTerms);
    if (header.indexedTotals.numTerms > INT32_MAX) io.invalid("ordinal width");
    auto count = OrdColumn::encodedCount(column.indexing, maxDoc, header.numValues);
    auto direct = OrdColumnFormat::directBytes(count, width);
    auto values = column.values();
    if (column.mode == OrdMode::DIRECT) {
      if (column.valuesBytes != direct) io.invalid("direct ordinal payload size");
      extents.add(io, values);
    } else {
      if (column.valuesBytes > direct) io.invalid("predicted ordinal payload size");
      extents.check(io, values);
      extents.add(io, values, ordBlockMetadata(column, count));
    }
  }

  void part(const Errors& io, const BytesColumn& column) {
    extents.add(io, column.values);
    if (header.type == FieldType::VECTOR) {
      if (column.endOffsets) io.invalid("vector column must use fixed stride");
      auto stride = bytesStride(column, header.numValues);
      if (stride <= 0 || vcodec::dimsFromBytes((VectorEncoding)column.vectorEncoding, stride) <= 0) {
        io.invalid("vector stride is not a whole vector in the column's encoding");
      }
    } else if (column.vectorEncoding) {
      io.invalid("vector encoding on a non-vector column");
    } else if (column.endOffsets) {
      mono(io, column.endOffsets, header.numValues, "end offsets index layout");
    } else if (bytesStride(column, header.numValues) < 0) {
      io.invalid("fixed bytes column size or stride");
    }
  }

  void part(const Errors& io, const Points& points) {
    auto leafCount = pointLeafCount(points, header.type, header.numValues);
    if (!points.leavesBytes) io.invalid("points part has no leaf bytes");
    if (!leafCount || leafCount > UINT32_MAX) io.invalid("points leaf count is out of range");
    extents.check(io, points.leaves());
    if (points.leaves().end().offset() % MAX_ALIGN) io.invalid("points directory alignment");
    extents.add(io, points.leaves(), pointsDirectory(points, header.type, header.numValues));
  }

  void part(const Errors& io, const Norms& norms) {
    if (norms.mode > NormsMode::LAST) io.invalid("unknown norms mode");
    if (norms.mode == NormsMode::SPARSE && header.docsWithField == maxDoc) {
      io.invalid("sparse norms require a docs bitmap");
    }
    extents.add(io, normsValues(norms, maxDoc, header.docsWithField));
  }

  void part(const Errors& io, const Stored& stored) {
    if (stored.numChunks <= 0) io.invalid("stored resource has no chunks");
    if (stored.numChunks > header.docsWithField) io.invalid("stored chunk count exceeds docs");
    if (stored.numDirtyChunks < 0 || stored.numDirtyChunks > stored.numChunks) io.invalid("dirty chunk count exceeds chunks");
    if (stored.maxChunkBytes < 0) io.invalid("stored max chunk bytes exceeds limit");
    if (stored.codec > StoredChunkFormat::Codec::LAST) io.invalid("stored codec");
    if (!stored.chunkTarget) io.invalid("zero stored chunk target");
    if (!stored.maxDocsPerChunk) io.invalid("zero stored docs per chunk");
    if (stored.dictionaryBytes && stored.codec != StoredChunkFormat::ZSTD) io.invalid("dictionary requires ZSTD");
    if (!stored.namesBytes) io.invalid("truncated stored names");
    extents.check(io, stored.chunks());
    if (stored.dictionaryBytes) extents.check(io, stored.dictionary());
    extents.add(io, stored.chunks(), stored.names());
    mono(io, stored.firstDocs, stored.numChunks, "first docs index layout");
    mono(io, stored.chunkOffsets, (int64_t)stored.numChunks + 1, "chunk offsets index layout");
  }

  void part(const Errors& io, FieldPart part, Slice slice) {
    switch (part) {
      case FieldPart::DOCS:
        extents.add(io, slice);
        if (!screaming::BitSet::validSize(slice.location.offset(), slice.bytes, header.docsWithField, maxDoc)) {
          io.invalid("docs bitmap size or alignment");
        }
        break;
      case FieldPart::VALUE_RANKS:
        mono(io, slice, header.docsWithField, "end-value ranks index layout");
        break;
      case FieldPart::OWNERS:
        mono(io, slice, header.numValues, "reverse owners index layout");
        break;
      case FieldPart::FILTER:
        extents.add(io, slice);
        if (slice.bytes < IdFilter::HEADER_BYTES) io.invalid("id filter size");
        break;
      default: std::unreachable();
    }
  }

  void finish(const Errors& io) { extents.finish(io); }
};

// Coverage of the terms, blocks and dictionary bytes across consecutive runs.
class TermRunCoverage {
  const uint64_t numTerms;  // Total terms across every run.
  const uint64_t dictionaryBytes;  // Total encoded dictionary block bytes.
  int32_t termOrd = 0;  // Ordinal at the next run start.
  int32_t blockOrd = 0;  // Block ordinal at the next run start.
  uint64_t dictionaryStart = 0;  // Bytes covered by previous runs.

public:
  TermRunCoverage(const Terms& terms, int64_t numTerms)
      : numTerms((uint64_t)numTerms), dictionaryBytes(terms.dictionaryBytes) {}

  TermRun start() const { return {termOrd, blockOrd, dictionaryStart, {}, {}}; }

  uint64_t remainingTerms() const { return numTerms - termOrd; }
  uint64_t remainingBytes() const { return dictionaryBytes - dictionaryStart; }

  void advance(const Errors& io, uint64_t terms, uint64_t bytes) {
    if (!terms || !bytes) io.invalid("term run is empty");
    if (terms > remainingTerms()) io.invalid("term run exceeds the remaining terms");
    if (bytes > remainingBytes()) io.invalid("term run exceeds the remaining dictionary bytes");
    if (bytes > std::numeric_limits<Terms::BlockOffset>::max()) {
      io.invalid("term run bytes exceed the block offset limit");
    }
    auto blocks = Postings::termBlockCount(terms);
    if (!Postings::fitsTermBlocks(blockOrd, blocks)) io.invalid(Postings::TERM_BLOCK_LIMIT_ERROR);
    static_assert((uint64_t)Postings::MAX_TERM_BLOCKS * Postings::TERMS_BLOCK_SIZE <= INT32_MAX);
    termOrd += (int32_t)terms;
    blockOrd += (int32_t)blocks;
    dictionaryStart += bytes;
  }
};

uint8_t streamMask(const TermRun& run) {
  return (run.docs ? StreamOptions::DOCS : 0) | (run.positions ? StreamOptions::POSITIONS : 0);
}

// The next run's start gives this run's count and length. The field gives the last one's end.
std::pair<int64_t, uint64_t> runSize(const Terms& terms, int64_t numTerms, int32_t i) {
  const auto& run = terms.runs[i];
  const auto* next = i + 1 == (int32_t)terms.runs.size() ? nullptr : &terms.runs[i + 1];
  return {(next ? next->firstTermOrd : numTerms) - run.firstTermOrd,
          (next ? next->dictionaryStart : terms.dictionaryBytes) - run.dictionaryStart};
}

void checkTerms(Writer& out, PartChecks& checks, const Terms& terms, const FieldRecordHeader& header) {
  auto numTerms = header.indexedTotals.numTerms;
  if (terms.runs.empty() || terms.runs.size() > Postings::MAX_TERM_BLOCKS) out.invalid("term run count");
  TermRunCoverage coverage(terms, numTerms);
  for (int32_t i = 0; i < (int32_t)terms.runs.size(); ++i) {
    const auto& run = terms.runs[i];
    auto start = coverage.start();
    if (run.firstTermOrd != start.firstTermOrd || run.firstBlockOrd != start.firstBlockOrd
        || run.dictionaryStart != start.dictionaryStart) out.invalid("term run start");
    auto [count, bytes] = runSize(terms, numTerms, i);
    coverage.advance(out, count, bytes);
    checks.streams(out, run);
  }
  checks.dictionary(out, terms);
}

// Tags through the largest stream mask represent a single run. Larger tags count runs.
void writeTerms(Writer& out, const Terms& terms, int64_t numTerms) {
  int32_t runCount = (int32_t)terms.runs.size();
  out.writeVarUInt(runCount == 1 ? streamMask(terms.runs.front()) : (uint64_t)runCount + StreamOptions::RUN_TAG_BASE);
  if (runCount > 1) {
    for (int32_t first = 0; first < runCount; first += StreamOptions::RUNS_PER_BYTE) {
      uint8_t masks = 0;
      for (int32_t i = 0; i < StreamOptions::RUNS_PER_BYTE && first + i < runCount; ++i) {
        masks |= streamMask(terms.runs[first + i]) << (StreamOptions::BITS * i);
      }
      out.writeByte(masks);
    }
  }
  out.writeVarUInt(terms.dictionaryBytes);
  out.writeVarUInt(terms.trieBytes);
  out.writeVarUInt(terms.trieRootOffset);
  for (int32_t i = 0; i + 1 < runCount; ++i) {
    auto [count, bytes] = runSize(terms, numTerms, i);
    out.writeVarUInt(count);
    out.writeVarUInt(bytes);
  }
  writeLocation(out, terms.location);
  for (const auto& run : terms.runs) {
    if (run.docs) writeSlice(out, run.docs);
    if (run.positions) writeSlice(out, run.positions);
  }
}

void readTerms(Reader& in, PartChecks& checks, Terms& terms, uint64_t tag, std::span<TermRun> runs,
               int64_t numTerms) {
  int32_t runCount = (int32_t)runs.size();
  std::span<const char> masks;
  if (runCount > 1) {
    masks = in.take(ceilDiv(runCount, StreamOptions::RUNS_PER_BYTE));
    auto tail = runCount % StreamOptions::RUNS_PER_BYTE;
    if (tail && (uint8_t)masks.back() >> (StreamOptions::BITS * tail)) in.invalid("unused term stream bits");
  }
  terms.dictionaryBytes = in.readVarUInt(seg_location::maxOffset());
  terms.trieBytes = in.readVarUInt(seg_location::maxOffset());
  terms.trieRootOffset = in.readVarUInt();
  terms.runs = runs;
  TermRunCoverage coverage(terms, numTerms);
  for (int32_t i = 0; i < runCount; ++i) {
    runs[i] = coverage.start();
    bool last = i + 1 == runCount;
    auto count = last ? coverage.remainingTerms() : in.readVarUInt();
    auto bytes = last ? coverage.remainingBytes() : in.readVarUInt();
    coverage.advance(in, count, bytes);
  }
  terms.location = readLocation(in);
  checks.dictionary(in, terms);
  for (int32_t i = 0; i < runCount; ++i) {
    uint8_t mask = runCount == 1 ? (uint8_t)tag
        : ((uint8_t)masks[i / StreamOptions::RUNS_PER_BYTE] >> (StreamOptions::BITS * (i % StreamOptions::RUNS_PER_BYTE))) & StreamOptions::ALL;
    auto& run = runs[i];
    if (mask & StreamOptions::DOCS) run.docs = readSlice(in);
    if (mask & StreamOptions::POSITIONS) run.positions = readSlice(in);
    checks.streams(in, run);
  }
}

void writePart(Writer& out, const NumberColumn& column) {
  writeLocation(out, column.location);
  out.writeVarUInt(column.blocksBytes);
  out.writeVarUInt(column.indexBytes);
}

void readPart(Reader& in, NumberColumn& column) {
  column.location = readLocation(in);
  column.blocksBytes = in.readVarUInt();
  column.indexBytes = in.readVarUInt();
}

void writePart(Writer& out, const OrdColumn& column) {
  out.writeByte((column.mode == OrdMode::PREDICTED ? OrdOptions::PREDICTED : 0)
      | (column.indexing == OrdIndexing::DOC_ID ? OrdOptions::DOC_INDEXED : 0));
  writeLocation(out, column.location);
  if (column.mode == OrdMode::PREDICTED) out.writeVarUInt(column.valuesBytes);
}

void readOrdColumn(Reader& in, OrdColumn& column, const FieldRecordHeader& header, int32_t maxDoc) {
  auto options = in.readOptions(OrdOptions::PREDICTED | OrdOptions::DOC_INDEXED);
  column.mode = options & OrdOptions::PREDICTED ? OrdMode::PREDICTED : OrdMode::DIRECT;
  column.indexing = options & OrdOptions::DOC_INDEXED ? OrdIndexing::DOC_ID : OrdIndexing::RANK;
  column.location = readLocation(in);
  if (column.mode == OrdMode::PREDICTED) {
    column.valuesBytes = in.readVarUInt();
  } else {
    auto bytes = OrdColumnFormat::directBytes(OrdColumn::encodedCount(column.indexing, maxDoc, header.numValues),
                                             OrdColumnFormat::width(header.indexedTotals.numTerms));
    if (bytes > seg_location::maxOffset()) in.invalid("direct ordinal payload exceeds the file offset limit");
    column.valuesBytes = (uint64_t)bytes;
  }
}

void writePart(Writer& out, const BytesColumn& column) {
  out.writeByte((column.endOffsets ? HAS_END_OFFSETS : 0) | (column.vectorEncoding ? HAS_VECTOR_ENCODING : 0));
  if (column.vectorEncoding) out.writeByte(column.vectorEncoding);
  writeSlice(out, column.values);
  if (column.endOffsets) writeSlice(out, column.endOffsets);
}

void readPart(Reader& in, BytesColumn& column) {
  auto options = in.readOptions(HAS_END_OFFSETS | HAS_VECTOR_ENCODING);
  if (options & HAS_VECTOR_ENCODING) {
    column.vectorEncoding = in.readByte();
    if (!column.vectorEncoding || !vcodec::valid(column.vectorEncoding)) in.invalid("unknown vector encoding");
  }
  column.values = readSlice(in);
  if (options & HAS_END_OFFSETS) column.endOffsets = readSlice(in);
}

void writePart(Writer& out, const Points& points) {
  writeLocation(out, points.location);
  out.writeVarUInt(points.leavesBytes);
  out.writeVarUInt(points.maxPointsPerLeaf);
}

void readPart(Reader& in, Points& points) {
  points.location = readLocation(in);
  points.leavesBytes = in.readVarUInt();
  points.maxPointsPerLeaf = (uint16_t)in.readVarUInt(UINT16_MAX);
}

void writePart(Writer& out, const Norms& norms) {
  out.writeByte(norms.mode == NormsMode::SPARSE ? SPARSE_NORMS : 0);
  writeLocation(out, norms.location);
}

void readPart(Reader& in, Norms& norms) {
  norms.mode = in.readOptions(SPARSE_NORMS) & SPARSE_NORMS ? NormsMode::SPARSE : NormsMode::FLAT;
  norms.location = readLocation(in);
}

void writePart(Writer& out, const Stored& stored) {
  out.writeVarUInt(stored.numChunks);
  out.writeVarUInt(stored.numDirtyChunks);
  out.writeVarUInt(stored.maxChunkBytes);
  out.writeByte((uint8_t)stored.codec);
  out.writeVarUInt(stored.chunkTarget);
  out.writeVarUInt(stored.maxDocsPerChunk);
  writeLocation(out, stored.location);
  out.writeVarUInt(stored.chunksBytes);
  out.writeVarUInt(stored.dictionaryBytes);
  out.writeVarUInt(stored.namesBytes);
  writeSlice(out, stored.firstDocs);
  writeSlice(out, stored.chunkOffsets);
}

void readPart(Reader& in, Stored& stored) {
  stored.numChunks = (int32_t)in.readVarUInt(INT32_MAX);
  stored.numDirtyChunks = (int32_t)in.readVarUInt(INT32_MAX);
  stored.maxChunkBytes = (int64_t)in.readVarUInt(INT64_MAX);
  stored.codec = readMode<StoredChunkFormat::Codec>(in, in.readByte(), "stored codec");
  stored.chunkTarget = (uint32_t)in.readVarUInt(UINT32_MAX);
  stored.maxDocsPerChunk = (uint32_t)in.readVarUInt(UINT32_MAX);
  stored.location = readLocation(in);
  stored.chunksBytes = (int64_t)in.readVarUInt(INT64_MAX);
  stored.dictionaryBytes = in.readVarUInt();
  stored.namesBytes = in.readVarUInt();
  stored.firstDocs = readSlice(in);
  stored.chunkOffsets = readSlice(in);
}

// Each slot can hold its descriptor without padding between parts.
template <class T>
constexpr size_t descriptorBytes() {
  static_assert(alignof(T) <= MAX_ALIGN && sizeof(T) % MAX_ALIGN == 0);
  static_assert(std::is_trivially_destructible_v<T>);
  return sizeof(T);
}

constexpr std::array partBytes{
    descriptorBytes<Terms>(), size_t(0), descriptorBytes<Points>(), descriptorBytes<Norms>(),
    descriptorBytes<Stored>(), descriptorBytes<Slice>(), descriptorBytes<Slice>(), descriptorBytes<Slice>(),
    descriptorBytes<Slice>()
};
static_assert(partBytes.size() == NUM_PARTS);

constexpr size_t columnBytes(ColumnKind kind) {
  switch (kind) {
    case ColumnKind::NUMBER: return descriptorBytes<NumberColumn>();
    case ColumnKind::ORD: return descriptorBytes<OrdColumn>();
    case ColumnKind::BYTES: return descriptorBytes<BytesColumn>();
    case ColumnKind::NONE: return 0;
  }
  std::unreachable();
}

constexpr size_t boundsBytes(BoundsKind kind) {
  switch (kind) {
    case BoundsKind::SCALAR: return descriptorBytes<ScalarBounds>();
    case BoundsKind::DICTIONARY: return descriptorBytes<DictionaryBounds>();
    case BoundsKind::GEO: return descriptorBytes<GeoBounds>();
    case BoundsKind::INTERVAL: return descriptorBytes<IntervalBounds>();
    case BoundsKind::NONE: return 0;
  }
  std::unreachable();
}

// Even a record with every part fits in the part-offset type before its runs.
static_assert([] {
  size_t bytes = sizeof(DecodedFieldRecord) + NUM_PARTS * sizeof(PartOffset) + MAX_ALIGN - 1
      + sizeof(IndexedTotals) + std::max({boundsBytes(BoundsKind::SCALAR), boundsBytes(BoundsKind::DICTIONARY),
                                        boundsBytes(BoundsKind::GEO), boundsBytes(BoundsKind::INTERVAL)})
      + std::max({columnBytes(ColumnKind::NUMBER), columnBytes(ColumnKind::ORD), columnBytes(ColumnKind::BYTES)});
  for (auto size : partBytes) bytes += size;
  return bytes <= std::numeric_limits<PartOffset>::max();
}());

// Offsets of the present descriptors, with an optional trailing array of term runs.
struct BlockLayout {
  uint32_t runsOffset;  // Bytes through the last part.
  std::array<PartOffset, NUM_PARTS> offsets = {};  // Byte offsets by part kind, zero if absent.

  BlockLayout(uint32_t start, FieldPartSet parts, ColumnKind kind) : runsOffset(start) {
    for (int i = 0; i < NUM_PARTS; ++i) {
      if (!parts.has((FieldPart)i)) continue;
      offsets[i] = (PartOffset)runsOffset;
      runsOffset += i == (int)FieldPart::COLUMN ? columnBytes(kind) : partBytes[i];
    }
  }

  uint32_t allocationBytes(size_t runCount) const noexcept {
    return (uint32_t)(runsOffset + runCount * sizeof(TermRun));
  }

  template <class T>
  T& place(char* base, FieldPart part) const {
    return *::new (base + offsets[(int)part]) T();
  }
};

template <class T>
void readCheckedPart(Reader& in, PartChecks& checks, T& part) {
  readPart(in, part);
  checks.part(in, part);
}

template <class T>
void outputPart(Writer& out, PartChecks* checks, const T& part) {
  if (checks) checks->part(out, part);
  else writePart(out, part);
}

void outputSlice(Writer& out, PartChecks* checks, FieldPart id, Slice slice) {
  if (checks) checks->part(out, id, slice);
  else writeSlice(out, slice);
}

// Run the same steps to validate before writing any bytes, then to emit the parts.
void outputParts(Writer& out, const IndexFieldInfo& field, PartChecks* checks) {
  const auto& header = field.header;
  auto parts = header.parts;
  if (parts.has(FieldPart::TERMS)) {
    if (checks) checkTerms(out, *checks, field.terms, header);
    else writeTerms(out, field.terms, header.indexedTotals.numTerms);
  }
  if (parts.has(FieldPart::COLUMN)) {
    switch (columnKind(header)) {
      case ColumnKind::NUMBER: outputPart(out, checks, field.number); break;
      case ColumnKind::ORD: outputPart(out, checks, field.ord); break;
      case ColumnKind::BYTES: outputPart(out, checks, field.bytes); break;
      case ColumnKind::NONE: std::unreachable();
    }
  }
  if (parts.has(FieldPart::POINTS)) outputPart(out, checks, field.points);
  if (parts.has(FieldPart::NORMS)) outputPart(out, checks, field.norms);
  if (parts.has(FieldPart::STORED)) outputPart(out, checks, field.stored);
  if (parts.has(FieldPart::DOCS)) outputSlice(out, checks, FieldPart::DOCS, field.docs);
  if (parts.has(FieldPart::VALUE_RANKS)) outputSlice(out, checks, FieldPart::VALUE_RANKS, field.endValueRanks);
  if (parts.has(FieldPart::OWNERS)) outputSlice(out, checks, FieldPart::OWNERS, field.owners);
  if (parts.has(FieldPart::FILTER)) outputSlice(out, checks, FieldPart::FILTER, field.idFilter);
}

} // namespace

void FieldRecordCodec::encode(OutputStream& stream, const IndexFieldInfo& fieldInfo, int32_t maxDoc) {
  Writer out(&stream, RECORD_OUTPUT_ERROR);
  const auto& header = fieldInfo.header;
  checkHeader(out, header, maxDoc);
  PartChecks checks(header, maxDoc, std::nullopt);
  outputParts(out, fieldInfo, &checks);
  checks.finish(out);

  out.writePackedTerm(header.name);
  out.writeVarUInt(header.type);
  out.writeVarUInt(header.flags);
  if (FieldType::hasTypeParameters(header.type)) out.writeVarUInt(header.typeParameters);
  out.writeVarUInt(header.parts.mask());
  bool explicitValues = header.parts.has(FieldPart::COLUMN) && header.numValues != header.docsWithField;
  bool hasBounds = !std::holds_alternative<std::monostate>(header.bounds);
  out.writeByte(header.segFlags | (hasBounds ? HAS_BOUNDS : 0) | (explicitValues ? EXPLICIT_NUM_VALUES : 0));
  out.writeVarUInt(header.docsWithField);
  if (explicitValues) out.writeVarUInt(header.numValues);
  if (header.flags & FieldType::INDEX_DOCS) {
    auto [numTerms, df, ttf] = header.indexedTotals;
    out.writeVarUInt(numTerms);
    out.writeVarUInt(df - numTerms);
    out.writeVarUInt(ttf - df);
  }
  if (hasBounds) {
    switch (boundsKind(header.type, header.flags)) {
      case BoundsKind::SCALAR: writeBounds(out, std::get<ScalarBounds>(header.bounds)); break;
      case BoundsKind::DICTIONARY: writeBounds(out, std::get<DictionaryBounds>(header.bounds)); break;
      case BoundsKind::GEO: writeBounds(out, std::get<GeoBounds>(header.bounds)); break;
      case BoundsKind::INTERVAL: writeBounds(out, std::get<IntervalBounds>(header.bounds)); break;
      case BoundsKind::NONE: std::unreachable();
    }
  }
  outputParts(out, fieldInfo, nullptr);
}

FieldRecordHeader FieldRecordCodec::fieldHeader(std::span<const char> bytes, int32_t maxDoc, size_t* consumed,
                                                SegmentReadContext context) {
  Reader in(bytes, Errors(RECORD_ERROR, context));
  auto header = readHeader(in);
  checkHeader(in, header, maxDoc);
  if (consumed) *consumed = bytes.size() - in.remaining().size();
  return header;
}

FieldRecordPtr FieldRecordCodec::decode(std::span<const char> bytes, int32_t maxDoc, FileSizes files, int32_t ordinal,
                                       SegmentReadContext context) {
  Reader in(bytes, Errors(RECORD_ERROR, context));
  auto header = readHeader(in);
  checkHeader(in, header, maxDoc);

  uint64_t tag = 0;
  int32_t runCount = 0;
  if (header.parts.has(FieldPart::TERMS)) {
    tag = in.readVarUInt();
    auto count = tag <= StreamOptions::ALL ? 1 : tag - StreamOptions::RUN_TAG_BASE;
    // Each non-final run has two varints in addition to the packed presence bits.
    if (count > Postings::MAX_TERM_BLOCKS || count > (uint64_t)header.indexedTotals.numTerms
        || count - 1 > in.remaining().size() / 2) in.invalid("term run count");
    runCount = (int32_t)count;
  }
  auto parts = header.parts;
  auto kind = columnKind(header);
  auto bounds = std::holds_alternative<std::monostate>(header.bounds)
      ? BoundsKind::NONE : boundsKind(header.type, header.flags);
  auto totalsOffset = DecodedFieldRecord::totalsOffset(parts);
  auto boundsOffset = DecodedFieldRecord::boundsOffset(parts, header.flags);
  BlockLayout layout(boundsOffset + boundsBytes(bounds), parts, kind);
  static_assert(alignof(TermRun) <= MAX_ALIGN && std::is_trivially_destructible_v<TermRun>);
  static_assert((uint64_t)Postings::MAX_TERM_BLOCKS * sizeof(TermRun)
                + std::numeric_limits<PartOffset>::max() <= UINT32_MAX);
  static_assert(std::is_trivially_destructible_v<DecodedFieldRecord>);
  auto* base = (char*)::operator new(layout.allocationBytes(runCount));
  FieldRecordPtr record(::new (base) DecodedFieldRecord(header, ordinal));

  auto* table = ::new (base + sizeof(DecodedFieldRecord)) PartOffset[parts.size()];
  for (auto offset : layout.offsets) {
    if (offset) *table++ = offset;
  }
  if (header.flags & FieldType::INDEX_DOCS) ::new (base + totalsOffset) IndexedTotals(header.indexedTotals);
  auto* boundsData = base + boundsOffset;
  switch (bounds) {
    case BoundsKind::SCALAR: ::new (boundsData) ScalarBounds(std::get<ScalarBounds>(header.bounds)); break;
    case BoundsKind::DICTIONARY: ::new (boundsData) DictionaryBounds(std::get<DictionaryBounds>(header.bounds)); break;
    case BoundsKind::GEO: ::new (boundsData) GeoBounds(std::get<GeoBounds>(header.bounds)); break;
    case BoundsKind::INTERVAL: ::new (boundsData) IntervalBounds(std::get<IntervalBounds>(header.bounds)); break;
    case BoundsKind::NONE: break;
  }

  PartChecks checks(header, maxDoc, files);
  if (parts.has(FieldPart::TERMS)) {
    auto& terms = layout.place<Terms>(base, FieldPart::TERMS);
    auto* runs = ::new (base + layout.runsOffset) TermRun[runCount];
    readTerms(in, checks, terms, tag, std::span(runs, (size_t)runCount), header.indexedTotals.numTerms);
  }
  if (parts.has(FieldPart::COLUMN)) {
    switch (kind) {
      case ColumnKind::NUMBER:
        readCheckedPart(in, checks, layout.place<NumberColumn>(base, FieldPart::COLUMN));
        break;
      case ColumnKind::ORD: {
        auto& column = layout.place<OrdColumn>(base, FieldPart::COLUMN);
        readOrdColumn(in, column, header, maxDoc);
        checks.part(in, column);
        break;
      }
      case ColumnKind::BYTES:
        readCheckedPart(in, checks, layout.place<BytesColumn>(base, FieldPart::COLUMN));
        break;
      case ColumnKind::NONE: std::unreachable();
    }
  }
  if (parts.has(FieldPart::POINTS)) {
    readCheckedPart(in, checks, layout.place<Points>(base, FieldPart::POINTS));
  }
  if (parts.has(FieldPart::NORMS)) {
    readCheckedPart(in, checks, layout.place<Norms>(base, FieldPart::NORMS));
  }
  if (parts.has(FieldPart::STORED)) {
    readCheckedPart(in, checks, layout.place<Stored>(base, FieldPart::STORED));
  }
  auto read = [&](FieldPart part) {
    auto& slice = layout.place<Slice>(base, part);
    slice = readSlice(in);
    checks.part(in, part, slice);
  };
  if (parts.has(FieldPart::DOCS)) read(FieldPart::DOCS);
  if (parts.has(FieldPart::VALUE_RANKS)) read(FieldPart::VALUE_RANKS);
  if (parts.has(FieldPart::OWNERS)) read(FieldPart::OWNERS);
  if (parts.has(FieldPart::FILTER)) read(FieldPart::FILTER);
  checks.finish(in);
  in.requireEnd();
  return record;
}

DecodedFieldRecord::DecodedFieldRecord(const FieldRecordHeader& header, int32_t ordinal)
    : name(header.name), numValues(header.numValues), type(header.type), flags(header.flags),
      typeParameters(header.typeParameters), docsWithField(header.docsWithField), ordinal(ordinal),
      parts(header.parts), segFlags(header.segFlags),
      hasBounds(!std::holds_alternative<std::monostate>(header.bounds)) {}

uint32_t DecodedFieldRecord::allocatedBytes() const noexcept {
  auto bounds = hasBounds ? boundsKind(type, flags) : BoundsKind::NONE;
  BlockLayout layout(boundsOffset() + boundsBytes(bounds), parts, columnKind(type, flags));
  const auto* dictionary = terms();
  return layout.allocationBytes(dictionary ? dictionary->runs.size() : 0);
}

} // namespace luxir
