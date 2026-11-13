#include "hc/hand_tracker_onnx.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
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

// A black square canvas with `src` embedded in it, plus the row offset at
// which `src` was placed. The offset is DERIVED from the image actually
// passed in, not from a file-scope constant: a fixture of a different height
// must either be centered correctly or fail the assertion below, never
// silently mis-centre and turn the pixel comparison into nonsense.
struct PaddedSquare {
  hc_test::OwnedImage image;
  int pad_rows;
};

// Embeds `src` into a black `src.width x src.width` square, centered
// vertically. This is, pixel for pixel, exactly what a correct letterbox of a
// landscape frame produces: the same content, with `pad_rows` rows of zeros
// above and below. See LetterboxMatchesPaddedSquareInput.
//
// Requires a landscape (or square) source: a portrait image would need
// horizontal padding instead, and `src.height > src.width` would index past
// the end of the canvas. Asserted rather than assumed.
PaddedSquare MakeBlackPaddedSquare(const hc::ImageView& src) {
  const int side = src.width;
  EXPECT_GE(src.width, src.height)
      << "MakeBlackPaddedSquare needs a landscape source";

  PaddedSquare out;
  out.pad_rows = (src.width - src.height) / 2;
  out.image.pixels.assign(static_cast<std::size_t>(side) * side * 3u, 0);
  for (int y = 0; y < src.height; ++y) {
    const std::uint8_t* s = src.data + static_cast<std::size_t>(y) * src.stride;
    std::uint8_t* d =
        out.image.pixels.data() +
        static_cast<std::size_t>(y + out.pad_rows) * side * 3u;
    std::memcpy(d, s, static_cast<std::size_t>(src.width) * 3u);
  }
  out.image.view.data = out.image.pixels.data();
  out.image.view.width = side;
  out.image.view.height = side;
  out.image.view.stride = side * 3;
  out.image.view.format = src.format;
  return out;
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
  const std::uint32_t first_id = obs[0].track_id;
  // Task 9 builds association on top of track_id, so pin its contract here:
  // 0 is reserved as "no track", and a track that was lost and re-acquired
  // must NOT reuse the old identity -- otherwise a consumer holding state
  // keyed by track_id would silently stitch two different hands together.
  EXPECT_NE(first_id, 0u);

  t.Process(blank.view, 1, obs);
  EXPECT_TRUE(obs.empty());

  t.Process(img.view, 2, obs);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_NE(obs[0].track_id, 0u);
  EXPECT_NE(obs[0].track_id, first_id)
      << "re-detection reused the lost track's id";
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
  // Pin the fixture's own geometry. The padding offset is derived from the
  // image, so a resized fixture stays self-consistent, but the absolute-pixel
  // arithmetic below is written against these dimensions and must fail
  // loudly, not drift, if the fixture is ever replaced.
  ASSERT_EQ(img.view.width, kFixtureW);
  ASSERT_EQ(img.view.height, kFixtureH);

  const auto padded = MakeBlackPaddedSquare(img.view);
  ASSERT_EQ(padded.image.view.width, kFixtureW);
  ASSERT_EQ(padded.image.view.height, kFixtureW);
  ASSERT_EQ(padded.pad_rows, (kFixtureW - kFixtureH) / 2);

  std::vector<HandObservation> a, b;
  HandTrackerOnnx t1(DefaultTestConfig(/*max_hands=*/1), m1);
  t1.Process(img.view, 0, a);
  HandTrackerOnnx t2(DefaultTestConfig(/*max_hands=*/1), m2);
  t2.Process(padded.image.view, 0, b);

  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);

  for (int i = 0; i < 21; ++i) {
    // Both normalized spaces back to the same absolute pixel grid: the
    // padded image's rows are offset by pad_rows relative to the fixture's.
    const float ax = a[0].landmarks[i].x * kFixtureW;
    const float ay = a[0].landmarks[i].y * kFixtureH;
    const float bx = b[0].landmarks[i].x * kFixtureW;
    const float by = b[0].landmarks[i].y * kFixtureW -
                     static_cast<float>(padded.pad_rows);
    // 6 px on a 640 px frame (~1%). Comfortably above the resampling and
    // float noise between two numerically distinct paths to the same crop,
    // and far below the tens of pixels an aspect distortion moves a hand.
    EXPECT_NEAR(ax, bx, 6.0f) << "landmark " << i << " x";
    EXPECT_NEAR(ay, by, 6.0f) << "landmark " << i << " y";
  }
}

TEST(HandTracker, RejectsOutOfRangeMaxHands) {
  // HISTORY, so the loosening here is not mistaken for a regression. Task 8
  // shipped this as RejectsConfigsOtherThanSingleHand: max_hands > 1 threw,
  // deliberately, so that Task 10 had to lift the guard consciously rather
  // than discover by experiment that the field was inert. Task 10 lifted it
  // (ruling T10-E) -- two hands are now really tracked, pinned by the
  // HandTrackerTwo suite -- so max_hands = 2, which is also
  // HandTrackerConfig's default, must now CONSTRUCT.
  //
  // What remains rejected is what is genuinely unsupported, and the bounds
  // are not arbitrary: < 1 is meaningless, and the upper bound comes from
  // hc::kMaxAssociationEntries, past which AssignDetectionsToSlots silently
  // ignores slots and detections instead of associating them.
  Metrics m;
  EXPECT_NO_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/1), m));
  EXPECT_NO_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/2), m));
  EXPECT_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/0), m),
               std::invalid_argument);
  EXPECT_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/-1), m),
               std::invalid_argument);
  // 2*max_hands - 1 must stay <= kMaxAssociationEntries (64), so 32 is the
  // last accepted value and 33 is the first rejected one. Pinning both sides
  // of the boundary, not just one, so an off-by-one in the check fails here.
  EXPECT_NO_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/32), m));
  EXPECT_THROW(HandTrackerOnnx(DefaultTestConfig(/*max_hands=*/33), m),
               std::invalid_argument);
}

TEST(HandTracker, DefaultConfigConstructs) {
  // The default HandTrackerConfig (max_hands = 2) used to throw. It is now
  // the supported two-hand path, which is the whole point of Task 10.
  Metrics m;
  hc::HandTrackerConfig cfg = DefaultTestConfig(/*max_hands=*/1);
  cfg.max_hands = hc::HandTrackerConfig{}.max_hands;
  ASSERT_EQ(cfg.max_hands, 2);
  EXPECT_NO_THROW(HandTrackerOnnx(cfg, m));
}
