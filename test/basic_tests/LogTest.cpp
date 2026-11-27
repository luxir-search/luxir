// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <sstream>
#include <thread>

#include "luxir/LuxirConfig.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/NodeSummary.h"
#include "luxir/util/log.h"
#include "spdlog/sinks/ostream_sink.h"

using namespace luxir;

namespace {

// Routes the default logger into a string for one test, then restores it.
class CapturedLog {
  std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
  spdlog::level::level_enum previousLevel = spdlog::get_level();

public:
  std::ostringstream out;
  explicit CapturedLog(std::string_view format) {
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "captured", std::make_shared<spdlog::sinks::ostream_sink_st>(out)));
    spdlog::set_level(spdlog::level::info);
    logs::setFormat(format);
  }
  ~CapturedLog() {
    spdlog::set_default_logger(previous);
    spdlog::set_level(previousLevel);
    logs::setFormat("text");
  }
};

}  // namespace

TEST(LogTest, jsonFormatEscapesTheMessageAndStampsUtc) {
  std::string line;
  {
    CapturedLog log("json");
    LOG_INFO("a \"quoted\" {}\nline", "value\\");
    line = log.out.str();
  }
  EXPECT_TRUE(line.starts_with(R"({"ts":")")) << line;
  EXPECT_NE(line.find(R"(Z","level":"info","thread":)"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("src":"LogTest.cpp:)"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("msg":"a \"quoted\" value\\\nline"})"), std::string::npos) << line;
}

TEST(LogTest, textFormatHasIsoTimestampAndSourceOnlyWhenKnown) {
  std::string line;
  {
    CapturedLog log("text");
    LOG_WARN("with source");
    spdlog::warn("without source");
    line = log.out.str();
  }
  // 2026-11-12T18:04:31.120488Z warning T123 LogTest.cpp:61 with source
  EXPECT_EQ(line[4], '-');
  EXPECT_EQ(line[10], 'T');
  EXPECT_EQ(line[26], 'Z');
  EXPECT_NE(line.find(" warning T"), std::string::npos) << line;
  EXPECT_NE(line.find(" LogTest.cpp:"), std::string::npos) << line;
  EXPECT_EQ(line.find(" : without source"), std::string::npos) << line;
}

TEST(LogTest, unknownFormatIsRejected) {
  EXPECT_THROW(logs::setFormat("xml"), std::invalid_argument);
  LuxirConfig config;
  config.log.format = "yaml";
  EXPECT_THROW(config.normalize(), std::invalid_argument);
}

TEST(LogTest, rateLimitHoldsBackRepeatsAndCountsThem) {
  auto& settings = logs::settings();
  int64_t saved = settings.rateLimitMs;
  settings.rateLimitMs = 50;
  logs::RateLimit limit;
  uint64_t skipped = 99;
  EXPECT_TRUE(limit.allow(skipped));
  EXPECT_EQ(skipped, 0u);
  EXPECT_FALSE(limit.allow(skipped));
  EXPECT_FALSE(limit.allow(skipped));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_TRUE(limit.allow(skipped));
  EXPECT_EQ(skipped, 2u);
  settings.rateLimitMs = saved;
}

TEST(LogTest, slowThresholds) {
  std::atomic<int64_t> threshold{-1};
  EXPECT_FALSE(logs::slow(threshold, UINT64_MAX));
  threshold = 0;
  EXPECT_TRUE(logs::slow(threshold, 0));
  threshold = 5;
  EXPECT_FALSE(logs::slow(threshold, 4'999'999));
  EXPECT_TRUE(logs::slow(threshold, 5'000'000));
}

TEST(LogTest, nodeSummaryReportsTheInterval) {
  LuxirConfig config;
  LuxirNode node(config);
  NodeSummary summary(node, std::chrono::hours(1));
  auto line = summary.report();
  EXPECT_TRUE(line.starts_with("node summary ")) << line;
  EXPECT_NE(line.find("search n=0 "), std::string::npos) << line;
  EXPECT_NE(line.find(" index_ram="), std::string::npos) << line;
}
