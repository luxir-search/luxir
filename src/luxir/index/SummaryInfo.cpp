// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SummaryInfo.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "luxir/api/build.h"

namespace luxir {

OwnedFieldSummary fromWire(const api::FieldSummary& w) {
  OwnedFieldSummary o;
  o.fieldname = std::string(w.fieldname);
  o.termMin.assign((const char*)w.term_min.data(), w.term_min.size());
  o.termMax.assign((const char*)w.term_max.data(), w.term_max.size());
  o.docsWithField = w.docs_with_field;
  o.numValues = w.num_values;
  o.sumDocFreq = w.sum_doc_freq;
  o.sumTotalTermFreq = w.sum_total_term_freq;
  o.nTerms = w.n_terms;
  o.numMin = w.num_min;
  o.numMax = w.num_max;
  o.type = w.type;
  o.summaryFlags = w.summary_flags;
  o.flags = w.flags;
  o.typeFlags = w.type_flags;
  o.segmentsWithField = w.segments_with_field;
  o.geoMinLat = w.geo_min_lat;
  o.geoMaxLat = w.geo_max_lat;
  o.geoMinLon = w.geo_min_lon;
  o.geoMaxLon = w.geo_max_lon;
  return o;
}

api::FieldSummary toWire(const OwnedFieldSummary& o) {
  api::FieldSummary w;
  w.fieldname = o.fieldname;
  w.term_min = ::hpp_proto::bytes_view((const std::byte*)o.termMin.data(), o.termMin.size());
  w.term_max = ::hpp_proto::bytes_view((const std::byte*)o.termMax.data(), o.termMax.size());
  w.docs_with_field = o.docsWithField;
  w.num_values = o.numValues;
  w.sum_doc_freq = o.sumDocFreq;
  w.sum_total_term_freq = o.sumTotalTermFreq;
  w.n_terms = o.nTerms;
  w.num_min = o.numMin;
  w.num_max = o.numMax;
  w.type = o.type;
  w.summary_flags = o.summaryFlags;
  w.flags = o.flags;
  w.type_flags = o.typeFlags;
  w.segments_with_field = o.segmentsWithField;
  w.geo_min_lat = o.geoMinLat;
  w.geo_max_lat = o.geoMaxLat;
  w.geo_min_lon = o.geoMinLon;
  w.geo_max_lon = o.geoMaxLon;
  return w;
}

std::span<const api::FieldSummary> summariesToWire(std::span<const OwnedFieldSummary> records,
                                                   std::pmr::memory_resource& mr) {
  std::span<const api::FieldSummary> result;
  auto* out = api::build::allocArray(result, records.size(), mr);
  for (size_t i = 0; i < records.size(); i++) out[i] = toWire(records[i]);
  return result;
}

std::vector<OwnedFieldSummary> summariesFromWire(std::span<const api::FieldSummary> records) {
  std::vector<OwnedFieldSummary> result;
  result.reserve(records.size());
  for (const auto& w : records) result.push_back(fromWire(w));
  return result;
}

namespace {

using F = SegmentSummary;

constexpr uint32_t kTermBoundFlags = F::HAS_TERM_BOUNDS | F::TERM_BOUNDS_TRUNCATED | F::TERM_MAX_UNBOUNDED;

void saturatingAdd(uint64_t& target, uint64_t value) noexcept {
  target = value > std::numeric_limits<uint64_t>::max() - target ? std::numeric_limits<uint64_t>::max()
                                                                  : target + value;
}

void dropTermBounds(OwnedFieldSummary& r) {
  r.summaryFlags &= ~kTermBoundFlags;
  r.termMin.clear();
  r.termMax.clear();
}

void dropNumeric(OwnedFieldSummary& r) {
  r.summaryFlags &= ~(uint32_t)F::HAS_NUMERIC;
  r.numMin = r.numMax = 0;
}

void dropGeo(OwnedFieldSummary& r) {
  r.summaryFlags &= ~(uint32_t)F::HAS_GEO_BOX;
  r.geoMinLat = r.geoMaxLat = r.geoMinLon = r.geoMaxLon = 0;
}

} // namespace

CollectionSummary CollectionSummary::aggregate(std::span<const SegmentSummary* const> summaries) {
  CollectionSummary result;
  for (const SegmentSummary* summary : summaries) {
    if (summary == nullptr || !summary->complete()) return result;  // INCOMPLETE, no records
  }
  struct State {
    OwnedFieldSummary record;
    bool numericUnknown = false;
    bool geoUnknown = false;
    bool termUnknown = false;
    bool numValuesUnknown = false;
    bool allEmpty = true;
    bool typeMismatch = false;
  };
  std::vector<State> states;
  auto find = [&](std::string_view name) -> State& {
    auto it = std::ranges::lower_bound(states, name, {},
                                       [](const State& s) -> std::string_view { return s.record.fieldname; });
    if (it == states.end() || it->record.fieldname != name) {
      State fresh;
      fresh.record.fieldname = std::string(name);
      it = states.insert(it, std::move(fresh));
    }
    return *it;
  };
  for (const SegmentSummary* summary : summaries) {
    for (const auto& w : summary->fields()) {
      State& s = find(w.fieldname);
      OwnedFieldSummary& r = s.record;
      if (r.segmentsWithField == 0) {
        r.type = w.type;
        r.typeFlags = w.type_flags;
      } else if (r.type != w.type || r.typeFlags != w.type_flags) {
        s.typeMismatch = true;
      }
      r.segmentsWithField++;
      r.flags |= w.flags;
      saturatingAdd(r.docsWithField, w.docs_with_field);
      saturatingAdd(r.numValues, w.num_values);
      saturatingAdd(r.sumDocFreq, w.sum_doc_freq);
      saturatingAdd(r.sumTotalTermFreq, w.sum_total_term_freq);
      saturatingAdd(r.nTerms, w.n_terms);
      r.summaryFlags |= w.summary_flags & (F::MULTI_VALUED | F::TERM_BOUNDS_TRUNCATED | F::TERM_MAX_UNBOUNDED);
      if (w.summary_flags & F::HAS_NUM_VALUES) r.summaryFlags |= F::HAS_NUM_VALUES;
      else s.numValuesUnknown = true;
      if (w.summary_flags & F::EMPTY) continue;  // holds no value: constrains no bound
      s.allEmpty = false;
      if (w.summary_flags & F::HAS_NUMERIC) {
        if ((r.summaryFlags & F::HAS_NUMERIC) == 0) {
          r.numMin = w.num_min;
          r.numMax = w.num_max;
          r.summaryFlags |= F::HAS_NUMERIC;
        } else {
          r.numMin = std::min(r.numMin, w.num_min);
          r.numMax = std::max(r.numMax, w.num_max);
        }
      } else {
        s.numericUnknown = true;
      }
      if (w.summary_flags & F::HAS_GEO_BOX) {
        if ((r.summaryFlags & F::HAS_GEO_BOX) == 0) {
          r.geoMinLat = w.geo_min_lat;
          r.geoMaxLat = w.geo_max_lat;
          r.geoMinLon = w.geo_min_lon;
          r.geoMaxLon = w.geo_max_lon;
          r.summaryFlags |= F::HAS_GEO_BOX;
        } else {
          r.geoMinLat = std::min(r.geoMinLat, w.geo_min_lat);
          r.geoMaxLat = std::max(r.geoMaxLat, w.geo_max_lat);
          r.geoMinLon = std::min(r.geoMinLon, w.geo_min_lon);
          r.geoMaxLon = std::max(r.geoMaxLon, w.geo_max_lon);
        }
      } else {
        s.geoUnknown = true;
      }
      if (w.summary_flags & F::HAS_TERM_BOUNDS) {
        std::string_view termMin((const char*)w.term_min.data(), w.term_min.size());
        std::string_view termMax((const char*)w.term_max.data(), w.term_max.size());
        if ((r.summaryFlags & F::HAS_TERM_BOUNDS) == 0) {
          r.termMin = std::string(termMin);
          r.termMax = std::string(termMax);
          r.summaryFlags |= F::HAS_TERM_BOUNDS;
        } else {
          if (termMin < r.termMin) r.termMin = std::string(termMin);
          if (termMax > r.termMax) r.termMax = std::string(termMax);
        }
      } else {
        s.termUnknown = true;
      }
    }
  }
  result.fields.reserve(states.size());
  for (auto& s : states) {
    OwnedFieldSummary& r = s.record;
    if (s.numericUnknown || s.typeMismatch) dropNumeric(r);
    if (s.geoUnknown || s.typeMismatch) dropGeo(r);
    if (s.termUnknown || s.typeMismatch) dropTermBounds(r);
    // One segment's unbounded maximum makes the union's unbounded too.
    if (r.summaryFlags & F::TERM_MAX_UNBOUNDED) r.termMax.clear();
    if ((r.summaryFlags & F::HAS_TERM_BOUNDS) == 0) dropTermBounds(r);
    if (s.numValuesUnknown) {
      r.summaryFlags &= ~(uint32_t)F::HAS_NUM_VALUES;
      r.numValues = 0;
    }
    // Mixed persisted types fail every request using the field (field()
    // reports them), so the record proves nothing a weight would not raise.
    if (s.allEmpty && !s.typeMismatch) r.summaryFlags |= F::EMPTY;
    result.fields.push_back(std::move(r));
  }
  result.complete = true;
  result.tier = Tier::FULL;
  return result;
}

size_t CollectionSummary::encodedBytes() const {
  std::pmr::monotonic_buffer_resource arena;
  api::IndexInfo info;
  info.field_summaries = summariesToWire(fields, arena);
  info.summary_complete = complete;
  std::vector<std::byte> out;
  if (!api::encode(info, out)) throw std::runtime_error("collection summary: encoding failed");
  return out.size();
}

void CollectionSummary::fit(size_t maxBytes) {
  if (!complete || encodedBytes() <= maxBytes) return;
  for (auto& r : fields) dropTermBounds(r);
  tier = Tier::NO_TERM_BOUNDS;
  if (encodedBytes() <= maxBytes) return;
  for (auto& r : fields) {
    dropNumeric(r);
    dropGeo(r);
  }
  tier = Tier::PRESENCE;
  if (encodedBytes() <= maxBytes) return;
  // Dropping a record would claim its field absent, so the aggregate is
  // kept, without records, as one that proves nothing.
  fields.clear();
  complete = false;
  tier = Tier::OVER_BUDGET;
}

CollectionSummary CollectionSummary::fromManifest(std::span<const api::FieldSummary> wire, bool complete) {
  CollectionSummary result;
  result.complete = complete;
  if (!complete) return result;
  result.fields = summariesFromWire(wire);
  result.tier = Tier::FULL;  // the stored form, whatever tier it was fitted to
  return result;
}

} // namespace luxir
