#include "hc/hand_tracker_onnx.hpp"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_image.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/metrics.hpp"

using hc::Handedness;
using hc::HandObservation;
using hc::HandTrackerOnnx;
using hc::Metrics;
using hc::Stage;
using hc_test::DefaultTestConfig;
using hc_test::LoadRawFixture;
using hc_test::SolidImage;

namespace {

// The committed fixture is 640x480; the no-hand images match its size so the
// only difference between the two cases is the content, not the geometry.
constexpr int kFixtureW = 640;
constexpr int kFixtureH = 480;
constexpr std::uint8_t kMidGray = 128;

// Rows of black padding a correct letterbox adds above (and below) the
// 640x480 fixture when squaring it: (640 - 480) / 2.
constexpr int kPadRows = (kFixtureW - kFixtureH) / 2;

// Embeds `src` into a black kFixtureW x kFixtureW square, centered
// vertically. This is, pixel for pixel, exactly what a correct letterbox of
// the 640x480 fixture produces: the same content, with kPadRows rows of
// zeros above and below. See LetterboxMatchesPaddedSquareInput.
hc_test::OwnedImage MakeBlackPaddedSquare(const hc::ImageView& src) {
  hc_test::OwnedImage img;
  const int side = src.width;
  img.pixels.assign(static_cast<std::size_t>(side) * side * 3u, 0);
  for (int y = 0; y < src.height; ++y) {
    const std::uint8_t* s = src.data + static_cast<std::size_t>(y) * src.stride;
    std::uint8_t* d =
        img.pixels.data() +
        static_cast<std::size_t>(y + kPadRows) * side * 3u;
    std::memcpy(d, s, static_cast<std::size_t>(src.width) * 3u);
  }
  img.view.data = img.pixels.data();
  img.view.width = side;
  img.view.height = side;
  img.view.stride = side * 3;
  img.view.format = src.format;
  return img;
}

}  // namespace

TEST(HandTracker, FindsOneHandInFixture) {
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;
  t.Process(img.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);
  for (const auto& p : obs[0].landmarks) {
    EXPECT_GE(p.x, 0.0f);
    EXPECT_LE(p.x, 1.0f);
    EXPECT_GE(p.y, 0.0f);
    EXPECT_LE(p.y, 1.0f);
  }
  EXPECT_GT(obs[0].presence, 0.5f);
  // Ruling T8-E: the Left/Right label order is NOT DETERMINED (models.md
  // "Handedness label order"), so this asserts only that a side was assigned,
  // never which side. Tasks 9/10 resolve the label map.
  EXPECT_NE(obs[0].handedness, Handedness::Unknown);
}

TEST(HandTracker, LandmarksAreNotAllIdentical) {
  // Guards the failure where the warp or the output mapping collapses every
  // landmark onto one point, which still satisfies the range assertions above.
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;
  t.Process(img.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);

  float min_x = obs[0].landmarks[0].x, max_x = min_x;
  float min_y = obs[0].landmarks[0].y, max_y = min_y;
  for (const auto& p : obs[0].landmarks) {
    min_x = std::fmin(min_x, p.x);
    max_x = std::fmax(max_x, p.x);
    min_y = std::fmin(min_y, p.y);
    max_y = std::fmax(max_y, p.y);
  }
  // A real hand spans a visible fraction of the frame in both axes. 2% is far
  // below any plausible hand and far above any floating-point noise, so this
  // fails loudly on a collapse and never flaps on a small hand.
  EXPECT_GT(max_x - min_x, 0.02f);
  EXPECT_GT(max_y - min_y, 0.02f);
}

TEST(HandTracker, SkipsPalmDetectorWhileTracking) {
  // Process the same fixture 5 times. Metrics::Get(Stage::PalmDetect).calls
  // must be 1, and Get(Stage::Landmark).calls must be 5. This is both the
  // accuracy and the performance mechanism in the spec, so pin it.
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;
  for (int i = 0; i < 5; ++i) {
    t.Process(img.view, i, obs);
    ASSERT_EQ(obs.size(), 1u) << "lost the track on frame " << i;
  }
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
  EXPECT_EQ(m.Get(Stage::Landmark).calls, 5u);
}

TEST(HandTracker, EmptyFrameYieldsNoObservationsAndNoError) {
  Metrics m;
  const auto blank = SolidImage(kFixtureW, kFixtureH, kMidGray);
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;
  ASSERT_NO_THROW(t.Process(blank.view, 0, obs));
  EXPECT_TRUE(obs.empty());
  // The detector ran and found nothing; it must not have been skipped.
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
}

TEST(HandTracker, ReDetectsAfterHandDisappears) {
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  const auto blank = SolidImage(kFixtureW, kFixtureH, kMidGray);
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;

  t.Process(img.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);

  t.Process(blank.view, 1, obs);
  EXPECT_TRUE(obs.empty());

  t.Process(img.view, 2, obs);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
  EXPECT_EQ(obs.size(), 1u);
}

TEST(HandTracker, ResetClearsTrackingState) {
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;

  t.Process(img.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);

  t.Reset();

  t.Process(img.view, 1, obs);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
  EXPECT_EQ(obs.size(), 1u);
}

TEST(HandTracker, PopulatesTimestampFromArgument) {
  Metrics m;
  const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;

  constexpr std::int64_t kTs1 = 1234567;
  t.Process(img.view, kTs1, obs);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_EQ(obs[0].timestamp_us, kTs1);

  // A second, different timestamp on the tracked (palm-detector-skipping)
  // path, so the field cannot be satisfied by a constant set at detect time.
  constexpr std::int64_t kTs2 = 9876543210;
  t.Process(img.view, kTs2, obs);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_EQ(obs[0].timestamp_us, kTs2);
}

TEST(HandTracker, LetterboxMatchesPaddedSquareInput) {
  // Ruling T8-B: the full-frame input to the palm detector must be
  // LETTERBOXED (aspect preserved, short side zero-padded), not stretched.
  //
  // This invariant is derivable a priori, without any ground truth about
  // where the hand actually is. Letterboxing the 640x480 fixture produces
  // exactly the same square the detector would see if the caller had handed
  // us a 640x640 frame with 80 rows of black above and below -- the warp's
  // BORDER_ZERO fill and literal black pixels are the same values. So both
  // inputs must yield the same landmarks in ABSOLUTE PIXEL terms.
  //
  // A stretch breaks this: the 4:3 frame would be squashed into the square
  // while the already-square padded frame would not, so the two runs would
  // see differently-shaped hands and land in different places. Without this
  // test, replacing SquareLong() with the raw full-frame rect passes the
  // entire rest of the suite.
  Metrics m1, m2;
  const auto img = LoadRawFixture("hand_rgb");
  const auto padded = MakeBlackPaddedSquare(img.view);
  ASSERT_EQ(padded.view.width, kFixtureW);
  ASSERT_EQ(padded.view.height, kFixtureW);

  std::vector<HandObservation> a, b;
  HandTrackerOnnx t1(DefaultTestConfig(/*max_hands=*/1), m1);
  t1.Process(img.view, 0, a);
  HandTrackerOnnx t2(DefaultTestConfig(/*max_hands=*/1), m2);
  t2.Process(padded.view, 0, b);

  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);

  for (int i = 0; i < 21; ++i) {
    // Both normalized spaces back to the same absolute pixel grid: the
    // padded image's rows are offset by kPadRows relative to the fixture's.
    const float ax = a[0].landmarks[i].x * kFixtureW;
    const float ay = a[0].landmarks[i].y * kFixtureH;
    const float bx = b[0].landmarks[i].x * kFixtureW;
    const float by = b[0].landmarks[i].y * kFixtureW - kPadRows;
    // 6 px on a 640 px frame (~1%). Comfortably above the resampling and
    // float noise between two numerically distinct paths to the same crop,
    // and far below the tens of pixels an aspect distortion moves a hand.
    EXPECT_NEAR(ax, bx, 6.0f) << "landmark " << i << " x";
    EXPECT_NEAR(ay, by, 6.0f) << "landmark " << i << " y";
  }
}
