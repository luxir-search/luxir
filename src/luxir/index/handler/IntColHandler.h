// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"

#include "luxir/index/DocStream.h"
#include "luxir/index/BKDWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/IntColWriter.h"
#include "luxir/index/PointsWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/schema/RangeBounds.h"
#include "luxir/util/geo.h"
#include "luxir/util/NumericUtils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <vector>

using namespace luxir;
namespace luxir::handler {

// Numeric indexing adds this 16-byte-per-value peak outside inverter.pool, so the
// inverter autoflush cap must leave headroom for the transient sort array.
struct FlushPoint {
  int64_t value;
  int32_t docid;
};

static_assert(sizeof(FlushPoint) == 16);

inline std::vector<FlushPoint> collectNumericPoints(
    Inverter& inverter, LongStream& values, DocStream& docs, int64_t count,
    IntStream* lengths = nullptr) {
  std::vector<FlushPoint> points;
  points.reserve((size_t)count);
  inverter.noteFlushWorkspace(points.capacity() * sizeof(FlushPoint));
  values.visitValues(inverter.pool, [&](int64_t value) { points.push_back({value, 0}); });
  std::optional<IntStream::Reader> sizes;
  if (lengths) sizes.emplace(*lengths, inverter.pool);
  size_t index = 0;
  docs.forEachDoc(inverter.pool, [&](int32_t docid) {
    int32_t length = sizes ? sizes->next() : 1;
    assert(length >= 0);
    for (int32_t i = 0; i < length; i++) {
      assert(index < points.size());
      points[index++].docid = docid;
    }
  });
  assert(index == points.size());
  assert(!sizes || sizes->eof());
  return points;
}

inline WrittenTerms writeNumericPostings(PostingsWriter& postingsWriter,
                                         SpaceOrdinal space, const PostingsWriter::IndexFieldInfo& field,
                                 std::vector<FlushPoint>& points) {
  sortPointsByValueDocid(std::span<FlushPoint>(points));
  TextWriter writer(postingsWriter);
  writer.startField(space, field);
  size_t begin = 0;
  while (begin < points.size()) {
    auto term = encodeNumericTerm(points[begin].value);
    char packed[PackedTerm::getMemSize(8)];
    PackedTerm::write(packed, term.data(), (uint32_t)term.size());
    writer.startTerm(PackedTerm(packed));
    size_t end = begin;
    int32_t previousDoc = -1;
    while (end < points.size() && points[end].value == points[begin].value) {
      int32_t doc = points[end++].docid;
      if (doc != previousDoc) writer.addDoc(doc, 1);
      previousDoc = doc;
    }
    writer.endTerm(PackedTerm(packed));
    begin = end;
  }
  return writer.endField();
}

inline field_parts::Points writePoints(PostingsWriter& postingsWriter,
                        std::vector<FlushPoint>& points) {
  sortPointsByValueDocid(std::span<FlushPoint>(points));
  auto output = postingsWriter.getOutputStream();
  PointsWriter writer(*output, PointsWriter::Options{});
  for (const auto& point : points) writer.addPoint(point.value, point.docid);
  return writer.finish(postingsWriter, std::move(output));
}

inline WrittenPoints writeRangePoints(PostingsWriter& postingsWriter,
                             std::vector<BasicBKDWriter<BKDInt64Traits>::Point>& points) {
  auto output = postingsWriter.getOutputStream();
  BasicBKDWriter<BKDInt64Traits> writer(*output);
  return writer.write(points, postingsWriter, std::move(output));
}

inline WrittenPoints writeGeoPoints(PostingsWriter& postingsWriter,
                           std::vector<BKDWriter::Point>& points) {
  auto output = postingsWriter.getOutputStream();
  BKDWriter writer(*output);
  return writer.write(points, postingsWriter, std::move(output));
}

inline WrittenNumberColumn writeNumberColumn(Inverter& inverter, LongStream& values,
                                              PostingsWriter::IndexFieldInfo& fieldInfo, bool hasEndValueRanks) {
  auto& postingsWriter = inverter.getPostingsWriter();
  auto out = postingsWriter.getOutputStream();
  bool numeric = FieldType::isNumericColumn(fieldInfo.header.type);
  bool writesPoints = (fieldInfo.header.flags & FieldType::INDEX_RANGE) != 0;
  IntColWriter writer(*out,
                      fieldColumnSketch(fieldInfo.header.type,
                                        IntColWriter::writesValueSketch(fieldInfo.header.type, hasEndValueRanks)),
                      fieldColumnCodecs(fieldInfo.header.type));
  auto writeAndFold = [&](auto& bounds, auto fold) {
    values.visitValues(inverter.pool, [&](int64_t value) {
      writer.addInt64(value);
      fold(value);
    });
    fieldInfo.setBounds(bounds);
  };
  if (!writesPoints && fieldInfo.header.type == FieldType::GEO_POINT) {
    field_parts::GeoBounds bounds;
    writeAndFold(bounds, [&](int64_t value) {
      bounds.include(geo::unpackLatitude(value), geo::unpackLongitude(value));
    });
  } else if (!writesPoints && fieldInfo.header.type == FieldType::INTERVAL) {
    field_parts::IntervalBounds bounds;
    bool low = true;
    writeAndFold(bounds, [&](int64_t value) {
      auto& axis = low ? bounds.low : bounds.high;
      axis.include(value, value);
      low = !low;
    });
    assert(low);
  } else {
    values.pushValues(inverter.pool, writer);
  }

  auto written = writer.finish(postingsWriter, std::move(out));
  // Packed geo and interval values have axis bounds, supplied by the fold or BKD writer.
  if (numeric && !written.bounds.empty()) fieldInfo.setBounds(written.bounds);
  return written;
}

//
// Info for one single valued column.
//
// IntColHandler and MultiIntColHandler serve the whole int-column family
// (INT, FLOAT, DOUBLE, DATE, GEO_POINT): the value a column stores is whatever
// FieldType::coerceColInt64 returns (raw ints, sortable bits, epoch millis),
// so the handlers themselves are encoding-blind.  Coercion failures throw,
// which the per-doc update path recovers by marking the doc failed.
//
class IntColHandler : public Inverter::IndexHandler {
  friend class Inverter;
  LongStream longStream;
  DocStream docs;
  int32_t numValues = 0;

public:
  IntColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType, space), longStream(inverter.pool),
      docs(inverter.pool) {
  }

  ~IntColHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    // explicit null means "no value", same as an absent field
    if (coerce::isNull(val)) return;
    indexSingle(inverter, fieldType->coerceColInt64(
        val, std::string_view(fieldName), inverter.coerceContext));
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, fieldType->coerceColInt64(
        coerce::scalarVal(int64), std::string_view(fieldName),
        inverter.coerceContext));
  }

  void indexSingle(Inverter& inverter, int64_t val) {
    longStream.addVal(inverter.pool, val);
    docs.addDoc(inverter.pool, inverter.getDoc());
    numValues++;
  }

  void flush(Inverter& inverter) override {
    flushIntCol(inverter);
  }

  // TODO: make static and pass everything needed so it's composable
  void flushIntCol(Inverter& inverter) {
    if (numValues == 0) {
      return;  // drop the field.
    }
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    // TODO: move this to postingsWriter method
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(space, fieldName);
    fieldInfo.header.type = fieldType->type();
    fieldInfo.header.flags = fieldType->segmentFlags();
    fieldInfo.header.typeParameters = fieldType->segmentTypeParameters();
    flushIntCol(inverter, fieldInfo);
  }

  // This is the version called directly from text field for norms
  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& tmpPool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    if (fieldType->indexed()) {
      auto points = collectNumericPoints(inverter, longStream, docs, numValues);
      fieldInfo.setPostings(writeNumericPostings(postingsWriter, space, fieldInfo, points));
    }

    auto column = writeNumberColumn(inverter, longStream, fieldInfo, false);

    if (fieldType->rangeIndexed()) {
      if (fieldType->type() == FieldType::GEO_POINT) {
        std::vector<BKDWriter::Point> points;
        points.reserve((size_t)numValues);
        inverter.noteFlushWorkspace(points.capacity() * sizeof(BKDWriter::Point)
            + BKDWriter::workspaceBytes(points.capacity()));
        longStream.visitValues(inverter.pool, [&](int64_t value) {
          points.push_back({geo::unpackLatitude(value),
                            geo::unpackLongitude(value), 0});
        });
        size_t pointIndex = 0;
        docs.forEachDoc(inverter.pool, [&](int32_t docid) {
          assert(pointIndex < points.size());
          points[pointIndex++].docid = docid;
        });
        assert(pointIndex == points.size());
        auto written = writeGeoPoints(postingsWriter, points);
        fieldInfo.setPoints(written.points);
        fieldInfo.setBounds(written.bounds);
      } else {
        auto points = collectNumericPoints(inverter, longStream, docs, numValues);
        fieldInfo.setPoints(writePoints(postingsWriter, points));
      }
    }

    auto presence = PresenceWriter::write(tmpPool, postingsWriter, space, numValues,
        [&](auto&& accept) { docs.forEachDoc(inverter.pool, accept); });
    fieldInfo.setColumn(WrittenColumn{column.column, column.numValues, presence, {}, {}, {}});

  }
};


// Numeric sets sort and deduplicate encoded values before appending. Arrays
// retain source order and repeats. Multi-valued GEO_POINT uses array semantics.
class MultiIntColHandler : public Inverter::IndexHandler {
  friend class Inverter;
  LongStream longStream;
  IntStream lengthStream;
  DocStream docs;
  std::vector<int64_t> docValues;  // per-doc scratch, reused across docs
  int64_t numValues = 0;
  int32_t docsWithField = 0;

public:
  MultiIntColHandler(Inverter& inverter, const std::string_view& fieldName,
                     const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType, space), longStream(inverter.pool),
      lengthStream(inverter.pool), docs(inverter.pool) {
  }

  ~MultiIntColHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    // expected kinds first: array form, then a single value
    if (std::holds_alternative<luxir::api::ArrInt>(val.kind)) {
      auto& arr = std::get<luxir::api::ArrInt>(val.kind).v;
      index(inverter, std::span<const int64_t>(arr.data(), arr.size()));
      return;
    }
    if (std::holds_alternative<int64_t>(val.kind)) {
      index(inverter, std::get<int64_t>(val.kind));
      return;
    }
    if (coerce::isNull(val)) return;
    // Validate/materialize before mutating any stream. A failed coercion may
    // retain scratch capacity, which still belongs to the inverter's budget.
    docValues.clear();
    auto account = scope_guard([&] { accountExtraRam(inverter, docValues.capacity() * sizeof(int64_t)); });
    bool wasArray = coerce::forEachElement(val, [&](const IndexVal& elem) {
      docValues.push_back(fieldType->coerceColInt64(
          elem, std::string_view(fieldName), inverter.coerceContext));
    });
    if (!wasArray) {
      docValues.push_back(fieldType->coerceColInt64(
          val, std::string_view(fieldName), inverter.coerceContext));
    }
    appendDocValues(inverter);
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::views::single(
        fieldType->coerceColInt64(coerce::scalarVal(int64), std::string_view(fieldName),
                                  inverter.coerceContext)));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    // indexMulti materializes every coerced value before appending any of them.
    indexMulti(inverter, vals | std::views::transform([this, &inverter](int64_t v) {
      return fieldType->coerceColInt64(coerce::scalarVal(v), std::string_view(fieldName),
                                       inverter.coerceContext);
    }));
  }

  void indexMulti(Inverter& inverter, std::ranges::input_range auto&& values) {
    docValues.clear();
    auto account = scope_guard([&] { accountExtraRam(inverter, docValues.capacity() * sizeof(int64_t)); });
    for (auto val : values) docValues.push_back(val);
    appendDocValues(inverter);
  }

  void appendDocValues(Inverter& inverter) {
    if (docValues.empty()) return;
    if (fieldType->multiSet()) {
      std::ranges::sort(docValues);
      docValues.erase(std::unique(docValues.begin(), docValues.end()), docValues.end());
    }
    for (int64_t val : docValues) longStream.addVal(inverter.pool, val);
    int64_t n = (int64_t)docValues.size();
    numValues += n;
    docs.addDoc(inverter.pool, inverter.getDoc());
    lengthStream.addVal(inverter.pool, n);
    docsWithField++;
  }

  void flush(Inverter& inverter) override {
    flushIntCol(inverter);
  }

  // TODO: make static and pass everything needed so it's composable
  void flushIntCol(Inverter& inverter) {
    if (numValues == 0) {
      return;  // drop the field.
    }
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    // TODO: move this to postingsWriter method
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(space, fieldName);
    fieldInfo.header.type = fieldType->type();
    fieldInfo.header.flags = fieldType->segmentFlags();
    fieldInfo.header.typeParameters = fieldType->segmentTypeParameters();
    flushIntCol(inverter, fieldInfo);
  }

  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& pool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    bool hasEndValueRanks = fieldInfo.header.type == FieldType::INTERVAL || numValues != docsWithField;

    if (fieldType->indexed()) {
      auto points = collectNumericPoints(inverter, longStream, docs, numValues, &lengthStream);
      fieldInfo.setPostings(writeNumericPostings(postingsWriter, space, fieldInfo, points));
    }

    auto column = writeNumberColumn(inverter, longStream, fieldInfo, hasEndValueRanks);

    if (fieldType->rangeIndexed()) {
      if (fieldType->type() == FieldType::INTERVAL) {
        // Values alternate min, max; each doc holds length / 2 points.
        std::vector<BasicBKDWriter<BKDInt64Traits>::Point> points;
        points.reserve((size_t)numValues / 2);
        inverter.noteFlushWorkspace(points.capacity() * sizeof(BasicBKDWriter<BKDInt64Traits>::Point)
            + BasicBKDWriter<BKDInt64Traits>::workspaceBytes(points.capacity()));
        bool low = true;
        int64_t min = 0;
        longStream.visitValues(inverter.pool, [&](int64_t value) {
          if (low) min = value;
          else points.push_back({min, value, 0});
          low = !low;
        });
        IntStream::Reader lengths(lengthStream, inverter.pool);
        size_t pointIndex = 0;
        docs.forEachDoc(inverter.pool, [&](int32_t docid) {
          assert(!lengths.eof());
          int32_t length = lengths.next();
          assert(length % 2 == 0);
          for (int32_t i = 0; i < length / 2; i++) {
            assert(pointIndex < points.size());
            points[pointIndex++].docid = docid;
          }
        });
        assert(pointIndex == points.size());
        auto written = writeRangePoints(postingsWriter, points);
        fieldInfo.setPoints(written.points);
        fieldInfo.setBounds(written.bounds);
      } else if (fieldType->type() == FieldType::GEO_POINT) {
        std::vector<BKDWriter::Point> points;
        points.reserve((size_t)numValues);
        inverter.noteFlushWorkspace(points.capacity() * sizeof(BKDWriter::Point)
            + BKDWriter::workspaceBytes(points.capacity()));
        longStream.visitValues(inverter.pool, [&](int64_t value) {
          points.push_back({geo::unpackLatitude(value),
                            geo::unpackLongitude(value), 0});
        });
        IntStream::Reader lengths(lengthStream, inverter.pool);
        size_t pointIndex = 0;
        docs.forEachDoc(inverter.pool, [&](int32_t docid) {
          assert(!lengths.eof());
          int32_t length = lengths.next();
          assert(length >= 0);
          for (int32_t i = 0; i < length; i++) {
            assert(pointIndex < points.size());
            points[pointIndex++].docid = docid;
          }
        });
        assert(lengths.eof());
        assert(pointIndex == points.size());
        auto written = writeGeoPoints(postingsWriter, points);
        fieldInfo.setPoints(written.points);
        fieldInfo.setBounds(written.bounds);
      } else {
        auto points = collectNumericPoints(inverter, longStream, docs, numValues, &lengthStream);
        fieldInfo.setPoints(writePoints(postingsWriter, points));
      }
    }

    auto presence = PresenceWriter::write(pool, postingsWriter, space, docsWithField,
        [&](auto&& accept) { docs.forEachDoc(inverter.pool, accept); });

    field_parts::Slice endValueRanks;
    if (hasEndValueRanks) {
      auto guard = pool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endValueRankWriter(pool, *out);
      int64_t endValueRank = 0;

      lengthStream.visitValues(inverter.pool, [&endValueRank, &endValueRankWriter](auto val) {
        endValueRank += val;
        endValueRankWriter.addInt64(endValueRank);
      });

      endValueRanks = endValueRankWriter.finish(postingsWriter, std::move(out));
    }
    fieldInfo.setColumn(WrittenColumn{column.column, column.numValues, presence, endValueRanks, {}, {}});
  }
};

class GeoPointHandler final : public IntColHandler {
public:
  GeoPointHandler(Inverter& inverter, const std::string_view& fieldName,
                  const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : IntColHandler(inverter, fieldName, fieldType, space) {}

  using IntColHandler::index;
  void index(Inverter& inverter, std::span<const GeoPoint> points) override {
    if (points.empty()) return;
    if (points.size() != 1) {
      throw DocumentError(fmt::format(
          "Field '{}' is single-valued but received multiple values",
          std::string_view(fieldName)));
    }
    indexSingle(inverter, geo::encodePoint(points[0].latitude, points[0].longitude));
  }
};

class MultiGeoPointHandler final : public MultiIntColHandler {
public:
  MultiGeoPointHandler(Inverter& inverter, const std::string_view& fieldName,
                       const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : MultiIntColHandler(inverter, fieldName, fieldType, space) {}

  using MultiIntColHandler::index;
  void index(Inverter& inverter, std::span<const GeoPoint> points) override {
    indexMulti(inverter, points | std::views::transform([](const GeoPoint& point) {
      return geo::encodePoint(point.latitude, point.longitude);
    }));
  }
};

// A range field value: {"gte"|"gt": a, "lte"|"lt": b}, either side optional.
// The column holds the encoded inclusive (min, max) pair (see RangeFieldType).
class RangeHandler final : public MultiIntColHandler {
  [[noreturn]] void invalid(std::string_view detail) const {
    throw DocumentError(fmt::format("field '{}': cannot coerce value to a range: {}",
                                    std::string_view(fieldName), detail));
  }

public:
  RangeHandler(Inverter& inverter, const std::string_view& fieldName,
               const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : MultiIntColHandler(inverter, fieldName, fieldType, space) {}

  void index(Inverter& inverter, int64_t int64) override {
    unused(inverter, int64);
    invalid("expected an object with gte/gt and lte/lt bounds");
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    unused(inverter, vals);
    invalid("expected an object with gte/gt and lte/lt bounds");
  }

  void index(Inverter& inverter, const IndexVal& val) override {
    if (coerce::isNull(val)) return;
    if (const auto* arr = std::get_if<luxir::api::ArrVal>(&val.kind)) {
      if (arr->v.empty()) return;
      if (arr->v.size() != 1) invalid("multi-valued range fields are not supported yet");
      index(inverter, arr->v[0]);
      return;
    }
    const auto* map = std::get_if<luxir::api::Map>(&val.kind);
    if (map == nullptr) {
      invalid(fmt::format("expected an object with gte/gt and lte/lt bounds, got {}",
                          coerce::describe(val)));
    }
    const IndexVal* gte = nullptr;
    const IndexVal* gt = nullptr;
    const IndexVal* lte = nullptr;
    const IndexVal* lt = nullptr;
    for (const auto& [key, value] : map->fields) {
      const IndexVal* bound = &*value;
      if (key == "gte") gte = bound;
      else if (key == "gt") gt = bound;
      else if (key == "lte") lte = bound;
      else if (key == "lt") lt = bound;
      else invalid(fmt::format("unknown key '{}' (expected gte, gt, lte or lt)", key));
    }
    auto& rangeType = (const RangeFieldType&)*fieldType;
    auto name = std::string_view(fieldName);
    auto bounds = foldRangeBounds(rangeBoundLiterals(gte, gt, lte, lt, name),
        rangeType.domainBounds(), [&](const api::Val& value, FieldType::LiteralUse use) {
          return rangeType.coerceIngestBound(value, use, name, inverter.coerceContext);
        });
    if (bounds.empty()) invalid("the lower bound is above the upper bound");
    std::array<int64_t, 2> pair{bounds.lo, bounds.hi};
    indexMulti(inverter, pair);
  }
};

} // namespace luxir::handler
