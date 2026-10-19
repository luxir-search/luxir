// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <span>
#include <string>
#include <string_view>
#include "luxir/api/luxir_types.hpp"

namespace luxir {

// Prometheus text exposition format 0.0.4. Groups families so HELP/TYPE
// appear once, before all samples of a family.
class PrometheusText {
  struct Family { std::string help, type, samples; };
  std::map<std::string, Family, std::less<>> families;
  Family& family(std::string_view name, std::string_view help, std::string_view type);
public:
  static std::string label(std::string_view key, std::string_view value);
  void sample(std::string_view name, std::string_view help, std::string_view type,
              double value, std::string_view labels = {});
  void histogram(std::string_view name, std::string_view help, std::span<const double> bounds,
                 const api::HistogramStats& histogram, std::string_view labels = {});
  std::string str() const;
};

inline constexpr std::string_view prometheusContentType = "text/plain; version=0.0.4; charset=utf-8";

// Render a gathered stats response (see gatherStats). Every sample comes from
// the response alone; StatsMetricsTest checks which response fields have none.
std::string renderPrometheus(const api::StatsResponse& stats);
} // namespace luxir
