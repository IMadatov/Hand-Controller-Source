#include "hc/metrics.hpp"

#include <gtest/gtest.h>
#include <cmath>

using hc::Metrics;
using hc::Stage;

TEST(Metrics, PercentilesOverKnownSamples) {
  Metrics m;
  for (int i = 1; i <= 100; ++i) m.Add(Stage::Landmark, static_cast<double>(i));
  const auto s = m.Get(Stage::Landmark);
  EXPECT_EQ(s.calls, 100u);
  EXPECT_NEAR(s.mean_ms, 50.5, 1e-9);
  EXPECT_NEAR(s.p50_ms, 50.0, 1.0);
  EXPECT_NEAR(s.p95_ms, 95.0, 1.0);
}

TEST(Metrics, BumpCountsWithoutDuration) {
  Metrics m; m.Bump(Stage::PalmDetect); m.Bump(Stage::PalmDetect);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
}

TEST(Metrics, EmptyStageReportsZeroCalls) {
  Metrics m;
  EXPECT_EQ(m.Get(Stage::Preprocess).calls, 0u);
}

TEST(Metrics, ResetClearsAllStages) {
  Metrics m;
  m.Add(Stage::PalmDetect, 10.0);
  m.Add(Stage::Landmark, 20.0);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
  EXPECT_EQ(m.Get(Stage::Landmark).calls, 1u);

  m.Reset();
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 0u);
  EXPECT_EQ(m.Get(Stage::Landmark).calls, 0u);
}

TEST(Metrics, FormatNamesEveryStageWithSamples) {
  Metrics m;
  m.Add(Stage::Preprocess, 10.0);
  m.Add(Stage::Landmark, 20.0);
  m.Add(Stage::Landmark, 30.0);

  const auto formatted = m.Format();
  // Stages with samples should appear in lowercase
  EXPECT_NE(formatted.find("preprocess"), std::string::npos);
  EXPECT_NE(formatted.find("landmark"), std::string::npos);
  // PalmDetect was never touched here (calls == 0), so it is omitted.
  EXPECT_EQ(formatted.find("palmdetect"), std::string::npos);
  // Should contain call count information
  EXPECT_NE(formatted.find("calls=1"), std::string::npos);
  EXPECT_NE(formatted.find("calls=2"), std::string::npos);
}

TEST(Metrics, FormatShowsBumpOnlyStageWithoutFakeDurations) {
  Metrics m;
  m.Bump(Stage::PalmDetect);
  m.Bump(Stage::PalmDetect);

  const auto formatted = m.Format();
  // A stage counted without durations must still be reported, with its count.
  EXPECT_NE(formatted.find("palmdetect"), std::string::npos);
  EXPECT_NE(formatted.find("calls=2"), std::string::npos);
  // ...but must not invent a 0.00 ms duration for it.
  EXPECT_EQ(formatted.find("0.00"), std::string::npos);
  // A stage that never ran at all stays out of the report entirely.
  EXPECT_EQ(formatted.find("associate"), std::string::npos);
}

TEST(Metrics, PercentileConventionForOneAndTwoSamples) {
  // Pin the interpolation convention exactly: linear interpolation between
  // order statistics at (p/100)*(n-1). The 100-sample test's +/-1.0
  // tolerance would hide an off-by-one here.
  Metrics one;
  one.Add(Stage::Landmark, 7.0);
  EXPECT_DOUBLE_EQ(one.Get(Stage::Landmark).p50_ms, 7.0);
  EXPECT_DOUBLE_EQ(one.Get(Stage::Landmark).p95_ms, 7.0);

  Metrics two;
  two.Add(Stage::Landmark, 10.0);
  two.Add(Stage::Landmark, 20.0);
  // idx = 0.5 -> halfway between the two samples.
  EXPECT_DOUBLE_EQ(two.Get(Stage::Landmark).p50_ms, 15.0);
  // idx = 0.95 -> 10.0*(1-0.95) + 20.0*0.95 = 0.5 + 19.0 = 19.5.
  EXPECT_DOUBLE_EQ(two.Get(Stage::Landmark).p95_ms, 19.5);
}

TEST(Metrics, ScopedStageRecordsExactlyOneSample) {
  Metrics m;
  {
    hc::ScopedStage scoped(m, Stage::CaptureWait);
  }
  const auto s = m.Get(Stage::CaptureWait);
  EXPECT_EQ(s.calls, 1u);
}

TEST(Metrics, ScopedStageOnlyRecordsSamplesForItsStage) {
  Metrics m;
  {
    hc::ScopedStage scoped(m, Stage::Preprocess);
  }
  EXPECT_EQ(m.Get(Stage::Preprocess).calls, 1u);
  EXPECT_EQ(m.Get(Stage::CaptureWait).calls, 0u);
}
