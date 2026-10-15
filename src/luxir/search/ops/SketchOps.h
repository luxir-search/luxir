// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <charconv>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <xxhash.h>

#include "luxir/reader/FieldReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/util/AtomicMerger.h"
#include "luxir/util/HyperLogLog.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/util/TDigest.h"

namespace luxir {

// Shared shape of the sketch metrics: one mergeable sketch per calculator,
// fed by every segment of the incoming domain and written once, either to
// the op's own Val or to its bucket slot under a facet.
template <typename Sketch>
class SketchOp : public SearchOp {
public:
  std::string_view field; // physical column
  FieldType::Type fieldType;

  struct State : MergeableData {
    Sketch sketch;
    State() = default; // AtomicMerger default; Calc installs a creator
    explicit State(Sketch sketch) : sketch(std::move(sketch)) {}
    static State* merge(State* a, State* b) {
      a->sketch.merge(b->sketch);
      return a;
    }
  };

  SketchOp(SearchRequest& req, std::string_view name, std::string_view field,
           FieldType::Type fieldType)
      : SearchOp(req, name), field(field), fieldType(fieldType) {}

  bool canEmitAsBucketChild() const override { return true; }

  virtual Sketch newSketch() const = 0;
  virtual void accumulate(Sketch& sketch, int32_t segnum, DocSet* domain) = 0;
  virtual void write(api::Val& target, Sketch& sketch,
                     std::pmr::memory_resource& mr) = 0;

  // Walk the domain's values of a numeric or date column.
  // callback(int64_t raw) per value; absent fields contribute nothing.
  template <typename F>
  void forEachRawValue(int32_t segnum, DocSet* domain, F&& callback) {
    auto& postings = req.reader->segments()[(size_t)segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(postings);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo info;
    fieldReader.readFieldInfo(info);
    IntColReader column(postings, info);
    int64_t missing = 0;
    forEachIntColValue(domain, column, postings.maxDoc(), missing,
                       [&](int32_t docid, int64_t raw) LUXIR_INLINE {
                         unused(docid);
                         callback(raw);
                       });
  }

  class Calc : public Calculator {
    SegmentMergeDriver<State> driver;

    SketchOp& thisOp() { return (SketchOp&)getOp(); }

    void emit(State& state) {
      auto& mr = op.req.lastResponse->mr;
      getTarget(nullptr, [&](api::Val& val) {
        routeTarget<api::ArrVal>(val, mr, [&](api::Val& target) {
          thisOp().write(target, state.sketch, mr);
        });
      });
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
        : Calculator(op, parent, slot, numSlots),
          driver(op.req.reader->segments().size(),
                 [this](std::unique_ptr<State> state) { emit(*state); }) {
      driver.setCreator([this]() { return new State(thisOp().newSketch()); });
    }

    api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      unused(resp);
      unused(sub);
      return nullptr;
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domain) override {
      unused(tg);
      assert(domain.isDeliverable());
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }
      driver.contribute([&](State& state) {
        thisOp().accumulate(state.sketch, segnum, domain.get());
      });
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1,
                               int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

// Approximate distinct count of a field's values (HyperLogLog with an exact
// phase below precision_threshold). Numeric and date fields hash their
// column values; string and id fields hash the term bytes of each distinct
// segment ordinal once.
class CardinalityOp final : public SketchOp<HyperLogLog> {
public:
  int64_t threshold;

  CardinalityOp(SearchRequest& req, std::string_view name,
                std::string_view field, FieldType::Type fieldType,
                int64_t threshold)
      : SketchOp(req, name, field, fieldType), threshold(threshold) {}

  size_t facetBucketResidentBytes() const override {
    return HyperLogLog::maxBytes(threshold);
  }

  HyperLogLog newSketch() const override { return HyperLogLog(threshold); }

  void accumulate(HyperLogLog& sketch, int32_t segnum,
                  DocSet* domain) override {
    if (fieldType == FieldType::STRING || fieldType == FieldType::ID) {
      accumulateTerms(sketch, segnum, domain);
      return;
    }
    forEachRawValue(segnum, domain, [&](int64_t raw) LUXIR_INLINE {
      sketch.add(HyperLogLog::mix((uint64_t)raw));
    });
  }

  void write(api::Val& target, HyperLogLog& sketch,
             std::pmr::memory_resource& mr) override {
    unused(mr);
    target.kind.emplace<int64_t>(sketch.estimate());
  }

private:
  void accumulateTerms(HyperLogLog& sketch, int32_t segnum, DocSet* domain) {
    auto& postings = req.reader->segments()[(size_t)segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(postings);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo info;
    fieldReader.readFieldInfo(info);
    if (info.nTerms <= 0) return;
    // Mark the segment ordinals the domain touches, then hash each one's
    // term once: per-document cost is a bit set, not a hash.
    std::vector<uint64_t> seen(((size_t)info.nTerms + 63) / 64, 0);
    OrdColReader column(postings, info);
    int64_t missing = 0;
    forEachOrdValue(domain, column, OrdMap::SegToGlobal::identity(),
                    postings.maxDoc(), missing,
                    [&](int32_t docid, int64_t ord) LUXIR_INLINE {
                      unused(docid);
                      seen[(size_t)ord >> 6] |= uint64_t{1} << (ord & 63);
                    });
    TermsEnum terms(poolGuard.pool(), postings, info);
    for (size_t word = 0; word < seen.size(); word++) {
      for (uint64_t bits = seen[word]; bits != 0; bits &= bits - 1) {
        int64_t ord = (int64_t)(word * 64 + (size_t)std::countr_zero(bits));
        terms.seekOrd(ord);
        auto term = (std::string_view)terms.term();
        sketch.add(XXH3_64bits(term.data(), term.size()));
      }
    }
  }
};

// Approximate percentiles, or percentile ranks, of a numeric or date field's
// values from a t-digest. Results are keyed by the formatted percent (or
// value) in request order, or returned as a [{key, value}] list when unkeyed.
class PercentilesOp final : public SketchOp<TDigest> {
public:
  std::vector<double> points; // percents in [0, 100], or rank values
  std::vector<std::string> labels;
  double compression;
  bool ranks;
  bool keyed;

  // ES-compatible key text: integral values keep one decimal ("50.0"),
  // others use the shortest round-trip form ("99.9").
  static std::string label(double value) {
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
      char buf[32];
      auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value,
                                     std::chars_format::fixed, 1);
      unused(ec);
      return std::string(buf, end);
    }
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    unused(ec);
    return std::string(buf, end);
  }

  PercentilesOp(SearchRequest& req, std::string_view name,
                std::string_view field, FieldType::Type fieldType,
                std::vector<double> points, double compression, bool ranks,
                bool keyed)
      : SketchOp(req, name, field, fieldType), points(std::move(points)),
        compression(compression), ranks(ranks), keyed(keyed) {
    labels.reserve(this->points.size());
    for (double point : this->points) labels.push_back(label(point));
  }

  size_t facetBucketResidentBytes() const override {
    return TDigest::maxBytes(compression);
  }

  TDigest newSketch() const override { return TDigest(compression); }

  void accumulate(TDigest& digest, int32_t segnum, DocSet* domain) override {
    switch (fieldType) {
      case FieldType::FLOAT:
        forEachRawValue(segnum, domain, [&](int64_t raw) LUXIR_INLINE {
          double value = (double)sortableInt32ToFloat((int32_t)raw);
          if (std::isfinite(value)) digest.add(value);
        });
        break;
      case FieldType::DOUBLE:
        forEachRawValue(segnum, domain, [&](int64_t raw) LUXIR_INLINE {
          double value = sortableInt64ToDouble(raw);
          if (std::isfinite(value)) digest.add(value);
        });
        break;
      default:
        forEachRawValue(segnum, domain, [&](int64_t raw) LUXIR_INLINE {
          digest.add((double)raw);
        });
        break;
    }
  }

  void write(api::Val& target, TDigest& digest,
             std::pmr::memory_resource& mr) override {
    digest.finish();
    auto setResult = [&](api::Val& slot, double point) {
      if (digest.empty()) {
        slot.kind.emplace<google::protobuf::NullValue>(
            google::protobuf::NullValue::NULL_VALUE);
      } else if (ranks) {
        slot.kind.emplace<double>(100.0 * digest.cdf(point));
      } else {
        slot.kind.emplace<double>(digest.quantile(point / 100.0));
      }
    };
    if (keyed) {
      auto& map = target.kind.emplace<api::Map>();
      for (size_t i = 0; i < points.size(); i++) {
        auto* slot = api::build::mapSlot<api::Val>(
            map.fields, points.size(), api::build::arenaStr(mr, labels[i]), mr);
        setResult(*slot, points[i]);
      }
      return;
    }
    auto& list = target.kind.emplace<api::ArrVal>();
    auto* entries = api::build::allocArray(list.v, points.size(), mr);
    for (size_t i = 0; i < points.size(); i++) {
      auto& map = entries[i].kind.emplace<api::Map>();
      auto* key = api::build::mapSlot<api::Val>(map.fields, 2, "key", mr);
      key->kind.emplace<double>(points[i]);
      auto* value = api::build::mapSlot<api::Val>(map.fields, 2, "value", mr);
      setResult(*value, points[i]);
    }
  }
};

} // namespace luxir
