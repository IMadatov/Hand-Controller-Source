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
  // Stage with no samples should not appear, or at least not as a misleading 0.00 ms row
  EXPECT_EQ(formatted.find("palmdetect"), std::string::npos);
  // Should contain call count information
  EXPECT_NE(formatted.find("1"), std::string::npos);
  EXPECT_NE(formatted.find("2"), std::string::npos);
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
