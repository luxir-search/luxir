// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Owning representation of summary records, and the conservative union that
// turns a snapshot's segment summaries into the collection aggregate stored
// in the manifest root (IndexInfo.field_summaries).
//
// The wire type luxir::api::FieldSummary is NON-OWNING; the writer keeps the
// aggregate in these structs and converts at the wire boundary like AuxInfo.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hpp_proto/field_types.hpp>

#include "luxir/api/build.h"
#include "luxir/api/luxir_index.hpp"
#include "SegmentSummary.h"

namespace luxir {

struct OwnedPayload {
  uint32_t kind = 0;
  uint32_t version = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t count = 0;
  uint64_t seed = 0;
  uint32_t param = 0;
};

struct OwnedFieldSummary {
  std::string fieldname;
  uint32_t type = 0;
  uint32_t summaryFlags = 0;
  uint64_t docsWithField = 0;
  uint64_t numValues = 0;
  uint64_t sumDocFreq = 0;
  uint64_t sumTotalTermFreq = 0;
  uint64_t nTerms = 0;
  int64_t numMin = 0;
  int64_t numMax = 0;
  int32_t geoMinLat = 0;
  int32_t geoMaxLat = 0;
  int32_t geoMinLon = 0;
  int32_t geoMaxLon = 0;
  std::string termMin;
  std::string termMax;
  std::vector<OwnedPayload> payloads;
  uint32_t segmentsWithField = 0;
  uint32_t flags = 0;
  uint32_t typeFlags = 0;
};

inline OwnedFieldSummary fromWire(const api::FieldSummary& w) {
  OwnedFieldSummary o;
  o.fieldname = std::string(w.fieldname);
  o.type = w.type;
  o.summaryFlags = w.summary_flags;
  o.docsWithField = w.docs_with_field;
  o.numValues = w.num_values;
  o.sumDocFreq = w.sum_doc_freq;
  o.sumTotalTermFreq = w.sum_total_term_freq;
  o.nTerms = w.n_terms;
  o.numMin = w.num_min;
  o.numMax = w.num_max;
  o.geoMinLat = w.geo_min_lat;
  o.geoMaxLat = w.geo_max_lat;
  o.geoMinLon = w.geo_min_lon;
  o.geoMaxLon = w.geo_max_lon;
  o.termMin.assign((const char*)w.term_min.data(), w.term_min.size());
  o.termMax.assign((const char*)w.term_max.data(), w.term_max.size());
  o.payloads.reserve(w.payloads.size());
  for (const auto& p : w.payloads) {
    o.payloads.push_back({p.kind, p.version, p.offset, p.length, p.count, p.seed, p.param});
  }
  o.segmentsWithField = w.segments_with_field;
  o.flags = w.flags;
  o.typeFlags = w.type_flags;
  return o;
}

// Views over `o` with span backing in `mr`; `o` and `mr` outlive the view.
inline api::FieldSummary toWire(const OwnedFieldSummary& o, std::pmr::memory_resource& mr) {
  api::FieldSummary w;
  w.fieldname = o.fieldname;
  w.type = o.type;
  w.summary_flags = o.summaryFlags;
  w.docs_with_field = o.docsWithField;
  w.num_values = o.numValues;
  w.sum_doc_freq = o.sumDocFreq;
  w.sum_total_term_freq = o.sumTotalTermFreq;
  w.n_terms = o.nTerms;
  w.num_min = o.numMin;
  w.num_max = o.numMax;
  w.geo_min_lat = o.geoMinLat;
  w.geo_max_lat = o.geoMaxLat;
  w.geo_min_lon = o.geoMinLon;
  w.geo_max_lon = o.geoMaxLon;
  w.term_min = ::hpp_proto::bytes_view((const std::byte*)o.termMin.data(), o.termMin.size());
  w.term_max = ::hpp_proto::bytes_view((const std::byte*)o.termMax.data(), o.termMax.size());
  auto* payloads = api::build::allocArray(w.payloads, o.payloads.size(), mr);
  for (size_t i = 0; i < o.payloads.size(); i++) {
    const auto& p = o.payloads[i];
    payloads[i].kind = p.kind;
    payloads[i].version = p.version;
    payloads[i].offset = p.offset;
    payloads[i].length = p.length;
    payloads[i].count = p.count;
    payloads[i].seed = p.seed;
    payloads[i].param = p.param;
  }
  w.segments_with_field = o.segmentsWithField;
  w.flags = o.flags;
  w.type_flags = o.typeFlags;
  return w;
}

inline std::span<const api::FieldSummary> toWire(std::span<const OwnedFieldSummary> records,
                                                 std::pmr::memory_resource& mr) {
  std::span<const api::FieldSummary> result;
  auto* out = api::build::allocArray(result, records.size(), mr);
  for (size_t i = 0; i < records.size(); i++) out[i] = toWire(records[i], mr);
  return result;
}

inline std::vector<OwnedFieldSummary> fromWire(std::span<const api::FieldSummary> records) {
  std::vector<OwnedFieldSummary> result;
  result.reserve(records.size());
  for (const auto& w : records) result.push_back(fromWire(w));
  return result;
}

// The conservative union of segment summaries: a record per physical field
// present in any segment, sorted by name, with no payloads. A bound survives
// only when every non-empty segment holding the field has it; counts always
// sum (saturating). `complete` is cleared when a segment has no summary or an
// incomplete one, because absence of a field is then no longer a proof.
class SummaryAggregator {
public:
  static std::vector<OwnedFieldSummary> aggregate(std::span<const std::shared_ptr<const SegmentSummary>> summaries,
                                                  bool& complete) {
    using F = SegmentSummary;
    struct State {
      OwnedFieldSummary record;
      bool numericUnknown = false;
      bool geoUnknown = false;
      bool termUnknown = false;
      bool numValuesUnknown = false;
      bool allEmpty = true;
      bool typeMismatch = false;
      bool first = true;
    };
    std::vector<State> states;
    auto find = [&](std::string_view name) -> State& {
      auto it = std::ranges::lower_bound(states, name, {}, [](const State& s) -> std::string_view { return s.record.fieldname; });
      if (it == states.end() || it->record.fieldname != name) {
        State fresh;
        fresh.record.fieldname = std::string(name);
        it = states.insert(it, std::move(fresh));
      }
      return *it;
    };
    auto saturatingAdd = [](uint64_t& target, uint64_t value) {
      target = value > std::numeric_limits<uint64_t>::max() - target ? std::numeric_limits<uint64_t>::max() : target + value;
    };
    complete = true;
    for (const auto& summary : summaries) {
      if (!summary || !summary->complete()) complete = false;
      if (!summary) continue;
      for (const auto& w : summary->fields()) {
        State& s = find(w.fieldname);
        OwnedFieldSummary& r = s.record;
        bool empty = (w.summary_flags & F::EMPTY) != 0;
        if (s.first) {
          r.type = w.type;
          r.typeFlags = w.type_flags;
          s.first = false;
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
        r.summaryFlags |= w.summary_flags & (F::MULTI_VALUED | F::TERM_BOUNDS_TRUNCATED | F::TERM_MAX_UNBOUNDED
                              | F::TERMS_OVERFLOW | F::MAY_HAVE_FITTED);
        if (w.summary_flags & F::HAS_NUM_VALUES) r.summaryFlags |= F::HAS_NUM_VALUES;
        else s.numValuesUnknown = true;
        if (empty) continue;
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
    std::vector<OwnedFieldSummary> result;
    result.reserve(states.size());
    for (auto& s : states) {
      OwnedFieldSummary& r = s.record;
      if (s.numericUnknown || s.typeMismatch) {
        r.summaryFlags &= ~(uint32_t)F::HAS_NUMERIC;
        r.numMin = r.numMax = 0;
      }
      if (s.geoUnknown || s.typeMismatch) {
        r.summaryFlags &= ~(uint32_t)F::HAS_GEO_BOX;
        r.geoMinLat = r.geoMaxLat = r.geoMinLon = r.geoMaxLon = 0;
      }
      if (s.termUnknown || s.typeMismatch) {
        r.summaryFlags &= ~(uint32_t)(F::HAS_TERM_BOUNDS | F::TERM_BOUNDS_TRUNCATED | F::TERM_MAX_UNBOUNDED);
        r.termMin.clear();
        r.termMax.clear();
      }
      if (r.summaryFlags & F::TERM_MAX_UNBOUNDED) r.termMax.clear();
      if (s.numValuesUnknown) {
        r.summaryFlags &= ~(uint32_t)F::HAS_NUM_VALUES;
        r.numValues = 0;
      }
      if (s.allEmpty) r.summaryFlags |= F::EMPTY;
      result.push_back(std::move(r));
    }
    return result;
  }
};

} // namespace luxir
