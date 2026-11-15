#include "overlay.hpp"

#include <array>
#include <limits>
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
TEST(Overlay, UnsupportedVisibilityWithALiveWindowIsNotAClose) {
  // The GTK build here returns -1 for the property on EVERY call, including
  // while the window is perfectly alive. The first version of this check read
  // -1 as "not visible" and quit after a single frame.
  EXPECT_FALSE(hc::WindowClosed(-1.0, /*image_width=*/640,
                                /*was_visible=*/true));
}

TEST(Overlay, WindowNotYetMappedIsNotAClose) {
  // Nothing has been seen alive yet. Creating a window and asking the same
  // instant whether it is gone is a race, not a close.
  EXPECT_FALSE(hc::WindowClosed(-1.0, /*image_width=*/-1,
                                /*was_visible=*/false));
  EXPECT_FALSE(hc::WindowClosed(0.0, /*image_width=*/-1,
                                /*was_visible=*/false));
}

TEST(Overlay, VisibleWindowIsNotClosed) {
  EXPECT_FALSE(hc::WindowClosed(1.0, /*image_width=*/640,
                                /*was_visible=*/true));
}

// THE CASE THAT WAS BROKEN IN BOTH DIRECTIONS, and the one a user hits by
// clicking the title bar's X. Measured on this build: a destroyed window
// still reports VISIBLE=-1 and a stale AUTOSIZE=1, and only the image rect
// goes negative. Before this, closing the window did nothing at all -- the
// next imshow recreated it and the loop ran on.
TEST(Overlay, AWindowThatWasAliveAndHasNoRectIsClosed) {
  EXPECT_TRUE(hc::WindowClosed(-1.0, /*image_width=*/-1,
                               /*was_visible=*/true));
}

// A backend that DOES report visibility is still honoured, so this does not
// become Linux-GTK-only behaviour by accident.
TEST(Overlay, AReportedInvisibleWindowIsStillAClose) {
  EXPECT_TRUE(hc::WindowClosed(0.0, /*image_width=*/640,
                               /*was_visible=*/true));
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

// --- Task 12: the per-hand gesture captions ------------------------------

namespace {

// Pixels written inside the band where a caption anchored to `obs` should
// land: above the hand's topmost landmark. Counting rather than sampling one
// pixel, because where exactly a glyph falls depends on the font.
int NonZeroAbove(const cv::Mat& bgr, Point2 anchor) {
  const int cx = static_cast<int>(anchor.x * kWidth);
  const int cy = static_cast<int>(anchor.y * kHeight);
  const int x0 = std::max(0, cx - 120);
  const int x1 = std::min(bgr.cols, cx + 120);
  const int y0 = std::max(0, cy - 32);
  const int y1 = std::max(0, cy);

  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (bgr.at<cv::Vec3b>(y, x) != cv::Vec3b(0, 0, 0)) ++count;
    }
  }
  return count;
}

}  // namespace

// The acceptance criterion for two-hand tracking, as seen on screen: both
// hands' gestures must be named at once, each beside its own hand.
TEST(Overlay, LabelsBothHandsAtTheirOwnPositions) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  const Point2 left_at{0.25f, 0.5f};
  const Point2 right_at{0.75f, 0.5f};
  const std::vector<HandObservation> obs{MakeObservation(0, left_at),
                                         MakeObservation(1, right_at)};
  const std::vector<hc::HandLabel> labels{{0, "thumb_up began"},
                                          {1, "point_index held"}};

  hc::DrawHandLabels(bgr, obs, labels);

  EXPECT_GT(NonZeroAbove(bgr, left_at), 0);
  EXPECT_GT(NonZeroAbove(bgr, right_at), 0);
}

// Each caption takes the colour of the hand it describes, so a glance pairs
// them up without reading the text.
TEST(Overlay, LabelTakesItsOwnHandsTrackColor) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);

  const std::vector<HandObservation> obs{MakeObservation(0, {0.25f, 0.5f}),
                                         MakeObservation(1, {0.75f, 0.5f})};
  hc::DrawHandLabels(bgr, obs, {{0, "fist held"}, {1, "fist held"}});

  // The same text on both hands, so a difference in the pixels can only come
  // from the colour.
  std::set<std::array<int, 3>> colors;
  for (int y = 0; y < bgr.rows; ++y) {
    for (int x = 0; x < bgr.cols; ++x) {
      const cv::Vec3b p = bgr.at<cv::Vec3b>(y, x);
      // Skip the black outline and the antialiased blend towards it.
      if (p[0] + p[1] + p[2] < 200) continue;
      colors.insert({p[0], p[1], p[2]});
    }
  }
  EXPECT_GE(colors.size(), 2u) << "both captions came out the same colour";
}

// The usual cause is a hand that left the frame on the same frame its gesture
// ended: there is an event, but no observation to anchor its caption to.
TEST(Overlay, LabelForAnAbsentTrackIsSkipped) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);
  const std::vector<HandObservation> obs{MakeObservation(0, {0.5f, 0.5f})};

  hc::DrawHandLabels(bgr, obs, {{99, "fist ended"}});

  EXPECT_EQ(cv::countNonZero(bgr.reshape(1)), 0);
}

TEST(Overlay, EmptyLabelTextDrawsNothing) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);
  const std::vector<HandObservation> obs{MakeObservation(0, {0.5f, 0.5f})};

  hc::DrawHandLabels(bgr, obs, {{0, ""}});

  EXPECT_EQ(cv::countNonZero(bgr.reshape(1)), 0);
}

// A hand at the very top of the frame: the caption has nowhere above to go,
// and putText CLIPS text drawn off-canvas rather than moving it -- so the
// label would vanish exactly when a user is most likely to wonder what it
// says.
TEST(Overlay, LabelStaysVisibleForAHandAtTheTopEdge) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);
  const std::vector<HandObservation> obs{MakeObservation(3, {0.5f, 0.0f})};

  hc::DrawHandLabels(bgr, obs, {{3, "open_palm held"}});

  EXPECT_GT(cv::countNonZero(bgr.reshape(1)), 0);
}

// Same for a hand against the right edge, where a centred caption would run
// off the side.
TEST(Overlay, LabelStaysVisibleForAHandAtTheRightEdge) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);
  const std::vector<HandObservation> obs{MakeObservation(3, {1.0f, 0.5f})};

  hc::DrawHandLabels(bgr, obs, {{3, "four_fingers began"}});

  EXPECT_GT(cv::countNonZero(bgr.reshape(1)), 0);
}

// Run under ASan: nothing may be written outside the frame buffer when the
// landmarks are not finite and there is nothing to anchor to.
TEST(Overlay, NonFiniteLandmarksLeaveTheLabelUndrawnWithoutCrashing) {
  cv::Mat bgr = cv::Mat::zeros(kHeight, kWidth, CV_8UC3);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<HandObservation> obs{MakeObservation(0, {nan, nan})};

  hc::DrawHandLabels(bgr, obs, {{0, "fist held"}});

  EXPECT_EQ(cv::countNonZero(bgr.reshape(1)), 0);
}
