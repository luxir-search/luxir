// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <sstream>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include "luxir/LuxirConfig.h"
#include "luxir/server/LuxirError.h"
#include "luxir/store/FileIOException.h"
#include "luxir/util/LogFormatter.h"

using namespace luxir;

TEST(LoggingTest, utcRecordsEscapeMultilinePayloads) {
  std::ostringstream output;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
  spdlog::logger logger("test", sink);
  logger.set_formatter(std::make_unique<LogFormatter>());
  logger.error("request_id=a\nb message=bad\r\t\033\\path");
  auto line = output.str();
  EXPECT_EQ(1, std::count(line.begin(), line.end(), '\n'));
  EXPECT_EQ('-', line.at(4));
  EXPECT_EQ('T', line.at(10));
  EXPECT_NE(std::string::npos, line.find("Z error T"));
  EXPECT_NE(std::string::npos, line.find("request_id=a\\nb message=bad\\r\\t\\x1b\\\\path"));
}

TEST(LoggingTest, internalStorageErrorsSurviveErrorThreshold) {
  LogLevelGuard level(spdlog::level::err);
  ErrorHolder errors;
  errors.context = {"update", "tenant", "collection", "request"};
  testing::internal::CaptureStdout();
  try { throw FileIOException("storage failure"); }
  catch (const std::exception& e) { errors.setException(e); }
  auto output = testing::internal::GetCapturedStdout();
  EXPECT_TRUE(errors.errored());
  EXPECT_EQ(ErrorKind::INTERNAL, errors.info().kind);
  EXPECT_NE(std::string::npos, output.find("storage failure"));
  EXPECT_NE(std::string::npos, output.find("tenant=tenant collection=collection request_id=request"));
}

TEST(LoggingTest, invalidLevelCannotDisableDiagnostics) {
  LuxirConfig config;
  CLI::App app;
  config.addOptions(app);
  const char* argv[] = {"luxir", "--log-level", "typo"};
  EXPECT_THROW(app.parse(3, argv), CLI::ValidationError);
  config.log_level = "typo";
  EXPECT_THROW(config.apply(), std::invalid_argument);
}

TEST(LoggingTest, allocationFailureIsNotBlamedOnRequestSyntax) {
  auto info = classifyException(std::bad_alloc(), ErrorKind::INVALID_REQUEST);
  EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, info.kind);
}
