#include "overlay.hpp"

#include <array>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"

using hc::DrawObservations;
using hc::Handedness;
using hc::HandObservation;
using hc::Point2;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// An observation with every landmark at the same normalized point. Tests then
// move only the landmarks they care about, so an assertion about one landmark
// is not accidentally satisfied by a neighbour.
HandObservation MakeObservation(std::uint32_t track_id, Point2 fill) {
  HandObservation obs{};
  obs.track_id = track_id;
  obs.handedness = Handedness::Right;
  obs.handedness_score = 1.0f;
  obs.presence = 1.0f;
  obs.landmarks.fill(fill);
  obs.timestamp_us = 0;
  return obs;
}

// True if any pixel within `radius` of (cx, cy) is nonzero. The tests ask
// "did something get drawn near here", which is the right precision for a
// drawing routine: the exact pixel depends on the marker's shape, the
// mapping of the centre does not.
bool AnyNonZeroNear(const cv::Mat& bgr, int cx, int cy, int radius) {
  const int x0 = std::max(0, cx - radius);
  const int y0 = std::max(0, cy - radius);
  const int x1 = std::min(bgr.cols - 1, cx + radius);
  const int y1 = std::min(bgr.rows - 1, cy + radius);
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      if (bgr.at<cv::Vec3b>(y, x) != cv::Vec3b(0, 0, 0)) return true;
    }
  }
  return false;
}

}  // namespace

// The one conversion in the project. 0.5 of the width and 0.25 of the height
// of a 640x480 frame is (320, 120); if width and height were ever swapped, or
// the factors dropped, this lands somewhere else entirely.
TEST(Overlay, DrawsLandmarksAtPixelPositionsFromNormalizedCoords) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  // The wrist at the point under test, every other landmark bunched into the
  // far corner, so only the wrist can satisfy the assertion.
  HandObservation obs = MakeObservation(0, Point2{0.9f, 0.9f});
  obs.landmarks[0] = Point2{0.5f, 0.25f};

  DrawObservations(bgr, std::vector<HandObservation>{obs});

  EXPECT_TRUE(AnyNonZeroNear(bgr, 320, 120, 4));

  // A swap of width and height would put the wrist at (240, 160); a control
  // point well away from both the wrist and the corner cluster stays black.
  EXPECT_FALSE(AnyNonZeroNear(bgr, 100, 400, 4));
}

TEST(Overlay, DrawsBothHandsInDistinctColors) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  // Two hands, well separated so neither one's drawing can colour the other's
  // centre pixel.
  const HandObservation left = MakeObservation(0, Point2{0.25f, 0.5f});
  const HandObservation right = MakeObservation(1, Point2{0.75f, 0.5f});

  DrawObservations(bgr, std::vector<HandObservation>{left, right});

  const cv::Vec3b left_color = bgr.at<cv::Vec3b>(240, 160);
  const cv::Vec3b right_color = bgr.at<cv::Vec3b>(240, 480);

  EXPECT_NE(left_color, cv::Vec3b(0, 0, 0));
  EXPECT_NE(right_color, cv::Vec3b(0, 0, 0));
  EXPECT_NE(left_color, right_color);
}

TEST(Overlay, EmptyObservationsLeaveFrameUnchanged) {
  // Deliberately not a black frame: a routine that cleared or reallocated the
  // Mat would pass against zeros.
  cv::Mat bgr(kHeight, kWidth, CV_8UC3);
  cv::randu(bgr, cv::Scalar::all(0), cv::Scalar::all(255));
  const cv::Mat before = bgr.clone();

  DrawObservations(bgr, std::vector<HandObservation>{});

  // NORM_INF over the difference: zero only if every channel of every pixel
  // is untouched. countNonZero cannot be used here, as it rejects a
  // three-channel input.
  EXPECT_EQ(cv::norm(bgr, before, cv::NORM_INF), 0.0);
}

// Run under -fsanitize=address: the point is not only that nothing crashes,
// but that nothing is written outside the frame buffer. The landmark is 1.4
// of the width and -0.2 of the height, i.e. off the right edge and above the
// top one, which is what a tracker emits when a hand leaves the view.
TEST(Overlay, HandlesLandmarksOutsideUnitRangeWithoutCrashing) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  HandObservation obs = MakeObservation(0, Point2{0.5f, 0.5f});
  obs.landmarks[0] = Point2{1.4f, -0.2f};

  DrawObservations(bgr, std::vector<HandObservation>{obs});

  // Clipped to the frame's corner rather than dropped, so an off-screen hand
  // is still visible as having gone off that particular edge.
  EXPECT_TRUE(AnyNonZeroNear(bgr, kWidth - 1, 0, 4));
}

// Every landmark off-frame in every direction, including far negative values
// that would wrap if the conversion ever went through an unsigned type.
TEST(Overlay, HandlesWildlyOutOfRangeLandmarksWithoutCrashing) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  HandObservation obs = MakeObservation(0, Point2{-50.0f, 50.0f});
  obs.landmarks[5] = Point2{1e9f, -1e9f};
  obs.landmarks[9] = Point2{0.5f, 0.5f};

  EXPECT_NO_FATAL_FAILURE(DrawObservations(bgr, std::vector<HandObservation>{obs}));
}

// Regression tests for a bug found only by running the app against a real
// camera: the preview window quit after exactly one frame.
//
// cv::getWindowProperty(WND_PROP_VISIBLE) returns -1 on this OpenCV build
// because the trimmed GTK backend does not implement the property. The
// original check was `prop < 1.0`, which -1 satisfies, so every frame
// reported "window closed". Render/ShouldQuit have no end-to-end test (they
// need a display), so this pins the decision itself.
TEST(Overlay, UnsupportedVisibilityPropertyIsNotAClose) {
  EXPECT_FALSE(hc::WindowClosed(-1.0, /*was_visible=*/false));
  EXPECT_FALSE(hc::WindowClosed(-1.0, /*was_visible=*/true));
}

TEST(Overlay, WindowNotYetMappedIsNotAClose) {
  // Supported property, but the compositor has not mapped the window yet.
  // Reading that as a close is the same one-frame-exit bug by another route.
  EXPECT_FALSE(hc::WindowClosed(0.0, /*was_visible=*/false));
}

TEST(Overlay, VisibleWindowIsNotClosed) {
  EXPECT_FALSE(hc::WindowClosed(1.0, /*was_visible=*/true));
}

TEST(Overlay, WindowThatWasVisibleAndIsNoLongerIsClosed) {
  // The actual close: seen visible, now reported not visible.
  EXPECT_TRUE(hc::WindowClosed(0.0, /*was_visible=*/true));
}

// Track ids are not indices: they come from the tracker's monotonic counter
// and grow without bound, so the colour lookup has to wrap rather than index
// a palette directly.
TEST(Overlay, LargeTrackIdSelectsAColorWithoutIndexingOutOfRange) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  const HandObservation obs = MakeObservation(4001u, Point2{0.5f, 0.5f});

  DrawObservations(bgr, std::vector<HandObservation>{obs});

  EXPECT_NE(bgr.at<cv::Vec3b>(240, 320), cv::Vec3b(0, 0, 0));
}
