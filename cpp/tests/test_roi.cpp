#include "hc/roi.hpp"

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "hc/geometry.hpp"
#include "hc/palm_decode.hpp"

using hc::DefaultLandmarkRoiOptions;
using hc::DefaultPalmRoiOptions;
using hc::LandmarkRoiOptions;
using hc::NormalizeRadians;
using hc::PalmDetection;
using hc::PalmRoiOptions;
using hc::Point2;
using hc::Rect;
using hc::RoiFromLandmarks;
using hc::RoiFromPalm;
using hc::RotatedRect;
using hc::WeightedLandmark;

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Builds a 21-landmark array with every point at a default "parked"
// location, so tests can override only the handful of indices they care
// about without worrying about the rest contaminating rotation or bbox
// computation in unexpected ways. The parked location sits inside the bbox
// formed by the test's explicit points, so it never affects BoundingBox
// unless a test deliberately overrides it.
std::array<Point2, 21> ParkedLandmarks(Point2 parked) {
  std::array<Point2, 21> lm;
  lm.fill(parked);
  return lm;
}

}  // namespace

TEST(Roi, NormalizeRadiansWrapsAtBoundaries) {
  // MediaPipe's NormalizeRadians is angle - 2*pi*floor((angle+pi)/(2*pi)),
  // which wraps into [-pi, pi): the upper boundary itself maps down to -pi
  // because floor rounds down exactly at that boundary.
  //   floor((pi + pi) / (2*pi)) = floor(1.0) = 1 -> pi - 2*pi*1 = -pi
  EXPECT_NEAR(NormalizeRadians(kPi), -kPi, 1e-6f);
  // -pi is already in range and stays put.
  EXPECT_NEAR(NormalizeRadians(-kPi), -kPi, 1e-6f);
  // Just over pi wraps to just over -pi.
  EXPECT_NEAR(NormalizeRadians(kPi + 0.1f), -kPi + 0.1f, 1e-5f);
  // Just under -pi wraps to just under +pi.
  EXPECT_NEAR(NormalizeRadians(-kPi - 0.1f), kPi - 0.1f, 1e-5f);
  // A large multiple of 2*pi plus a small angle wraps back to that angle.
  EXPECT_NEAR(NormalizeRadians(4.0f * kPi + 0.2f), 0.2f, 1e-4f);
  // Zero is unaffected.
  EXPECT_NEAR(NormalizeRadians(0.0f), 0.0f, 1e-6f);
}

TEST(Roi, VerticalHandGivesZeroRotation) {
  // Ruling T7-F: wrist (kp0) below MCP (kp2) in image coordinates (y grows
  // downward), dx == 0.
  //   y0 = 0.7 (wrist), y1 = 0.3 (MCP) => -(y1 - y0) = -(0.3 - 0.7) = 0.4 > 0
  //   dx = (x1 - x0) * aspect = 0 * aspect = 0
  //   atan2(0.4, 0) = pi/2
  //   rotation = NormalizeRadians(target_angle_rad(pi/2) - pi/2) = 0
  PalmDetection d{};
  d.box = Rect{0.3f, 0.2f, 0.4f, 0.4f};
  d.keypoints[0] = Point2{0.5f, 0.7f};  // wrist center
  d.keypoints[2] = Point2{0.5f, 0.3f};  // middle finger MCP
  d.score = 1.0f;

  const PalmRoiOptions o = DefaultPalmRoiOptions();
  const RotatedRect roi = RoiFromPalm(d, /*aspect=*/1.0f, o);
  EXPECT_NEAR(roi.rotation_rad, 0.0f, 1e-3f);
}

TEST(Roi, RotatingInputRotatesRoi) {
  // All 21 landmarks placed so the unrotated case gives rotation == 0 (same
  // vertical-axis construction as VerticalHandGivesZeroRotation), then every
  // point is rotated 90 degrees about (0.5, 0.5) using (dx, dy) -> (-dy, dx)
  // (aspect == 1, so this is a geometric rotation in the same pixel-like
  // units the ROI math uses). The whole configuration -- wrist, rotation
  // targets, and bbox subset -- rotates together, so both the ROI's
  // rotation and its center must advance by pi/2 while w/h stay the same.
  auto rotate90 = [](Point2 p) {
    const float dx = p.x - 0.5f;
    const float dy = p.y - 0.5f;
    return Point2{0.5f - dy, 0.5f + dx};
  };

  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.5f});
  lm[0] = Point2{0.4f, 0.7f};   // wrist
  lm[5] = Point2{0.4f, 0.3f};   // index MCP
  lm[9] = Point2{0.4f, 0.3f};   // middle MCP
  lm[13] = Point2{0.4f, 0.3f};  // ring MCP
  // bbox subset corners (rest of the 12-point subset), symmetric about wrist
  // x so rotation axis math isn't disturbed.
  lm[1] = Point2{0.2f, 0.4f};
  lm[2] = Point2{0.6f, 0.4f};
  lm[3] = Point2{0.2f, 0.6f};
  lm[6] = Point2{0.6f, 0.6f};
  lm[10] = Point2{0.2f, 0.5f};
  lm[14] = Point2{0.6f, 0.5f};
  lm[17] = Point2{0.3f, 0.5f};
  lm[18] = Point2{0.5f, 0.5f};

  std::array<Point2, 21> lm_rotated;
  for (std::size_t i = 0; i < lm.size(); ++i) {
    lm_rotated[i] = rotate90(lm[i]);
  }

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const RotatedRect base = RoiFromLandmarks(lm, /*aspect=*/1.0f, o);
  const RotatedRect rotated = RoiFromLandmarks(lm_rotated, /*aspect=*/1.0f, o);

  ASSERT_NEAR(base.rotation_rad, 0.0f, 1e-3f);
  EXPECT_NEAR(rotated.rotation_rad, kPi / 2.0f, 1e-3f);

  // The center (0.4, 0.5) before rotation maps under rotate90 to (0.5, 0.4).
  const Point2 expected_center = rotate90(base.center);
  EXPECT_NEAR(rotated.center.x, expected_center.x, 1e-3f);
  EXPECT_NEAR(rotated.center.y, expected_center.y, 1e-3f);

  // Dimensions are invariant under a 90-degree rotation at aspect == 1.
  EXPECT_NEAR(rotated.w, base.w, 1e-3f);
  EXPECT_NEAR(rotated.h, base.h, 1e-3f);
}

TEST(Roi, RoiIsSquareInPixelsWhenSquareLongSet) {
  // aspect 16/9, square_long true (both default option sets set it): after
  // TransformRect's internal squaring and then scale_x == scale_y uniform
  // scaling, roi.h (normalized to height) and roi.w (normalized to width)
  // must satisfy h / w == aspect, i.e. the crop is square in pixels.
  PalmDetection d{};
  d.box = Rect{0.3f, 0.35f, 0.4f, 0.1f};  // deliberately non-square box
  d.keypoints[0] = Point2{0.5f, 0.6f};
  d.keypoints[2] = Point2{0.5f, 0.4f};
  d.score = 1.0f;

  const PalmRoiOptions o = DefaultPalmRoiOptions();
  const RotatedRect roi = RoiFromPalm(d, 16.0f / 9.0f, o);
  EXPECT_NEAR(roi.h / roi.w, 16.0f / 9.0f, 1e-4f);
}

TEST(Roi, ExpansionMatchesConfiguredScale) {
  // 12-point bbox subset spans x in [0.3, 0.5], y in [0.2, 0.4] -> a 0.2 x
  // 0.2 box, already square at aspect 1 so square_long is a no-op. Rotation
  // axis: wrist (lm[0]) at (0.4, 0.4), target average at (0.4, 0.2) (index
  // MCP lm[5], middle MCP lm[9], ring MCP lm[13] all placed there) ->
  // dx == 0, dy = -(0.2 - 0.4) = 0.2 > 0 => atan2(0.2, 0) = pi/2,
  // rotation = pi/2 - pi/2 = 0, so the shift below applies along the
  // unrotated y axis directly.
  //
  // TransformRect step order (pre-square/pre-scale shift, matching ruling
  // T7-A):
  //   dy = h * shift_y = 0.2 * (-0.1) = -0.02  (shift_y from
  //        DefaultLandmarkRoiOptions, rotation == 0 so cos=1,sin=0)
  //   bbox center = (0.4, 0.3) -> shifted center = (0.4, 0.28)
  //   square_long: box already square in pixels at aspect 1, no change.
  //   scale: w = 0.2 * 2.0 = 0.4, h = 0.2 * 2.0 = 0.4 (scale_x == scale_y ==
  //        2.0 from DefaultLandmarkRoiOptions)
  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.3f});
  lm[0] = Point2{0.4f, 0.4f};   // wrist
  lm[5] = Point2{0.4f, 0.2f};   // index MCP
  lm[9] = Point2{0.4f, 0.2f};   // middle MCP
  lm[13] = Point2{0.4f, 0.2f};  // ring MCP
  lm[1] = Point2{0.3f, 0.2f};
  lm[2] = Point2{0.5f, 0.2f};
  lm[3] = Point2{0.3f, 0.4f};
  lm[6] = Point2{0.5f, 0.4f};
  lm[10] = Point2{0.3f, 0.3f};
  lm[14] = Point2{0.5f, 0.3f};
  lm[17] = Point2{0.35f, 0.3f};
  lm[18] = Point2{0.45f, 0.3f};

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const RotatedRect roi = RoiFromLandmarks(lm, /*aspect=*/1.0f, o);

  EXPECT_NEAR(roi.rotation_rad, 0.0f, 1e-3f);
  EXPECT_NEAR(roi.center.x, 0.4f, 1e-4f);
  EXPECT_NEAR(roi.center.y, 0.28f, 1e-4f);
  EXPECT_NEAR(roi.w, 0.4f, 1e-4f);
  EXPECT_NEAR(roi.h, 0.4f, 1e-4f);
}

TEST(Roi, PalmAndLandmarkOptionsDifferAndBothApply) {
  // Same geometry (a detection box plus landmarks covering an equivalent
  // extent) through both default option sets must give different rects,
  // since scale (2.6 vs 2.0) and shift_y (-0.5 vs -0.1) differ. This guards
  // against wiring one constant set into both paths.
  PalmDetection d{};
  d.box = Rect{0.3f, 0.3f, 0.2f, 0.2f};
  d.keypoints[0] = Point2{0.5f, 0.7f};
  d.keypoints[2] = Point2{0.5f, 0.3f};
  d.score = 1.0f;

  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.4f});
  lm[0] = Point2{0.5f, 0.7f};
  lm[5] = Point2{0.5f, 0.3f};
  lm[9] = Point2{0.5f, 0.3f};
  lm[13] = Point2{0.5f, 0.3f};
  lm[1] = Point2{0.3f, 0.3f};
  lm[2] = Point2{0.5f, 0.3f};
  lm[3] = Point2{0.3f, 0.5f};
  lm[6] = Point2{0.5f, 0.5f};
  lm[10] = Point2{0.3f, 0.4f};
  lm[14] = Point2{0.5f, 0.4f};
  lm[17] = Point2{0.35f, 0.4f};
  lm[18] = Point2{0.45f, 0.4f};

  const RotatedRect palm_roi =
      RoiFromPalm(d, /*aspect=*/1.0f, DefaultPalmRoiOptions());
  const RotatedRect lm_roi =
      RoiFromLandmarks(lm, /*aspect=*/1.0f, DefaultLandmarkRoiOptions());

  // Exact expected rects, so a cross-wired constant set actually fails here
  // (EXPECT_NE alone would not: feeding the palm's scale 2.6 into the
  // landmark path gives w = 1.04, still != the palm's 0.52).
  //
  // Palm path: box (0.3, 0.3, 0.2, 0.2) -> center (0.4, 0.4), w = h = 0.2.
  //   rotation: kp0 (0.5, 0.7), kp2 (0.5, 0.3) -> dx = 0,
  //     dy = -(0.3 - 0.7) = 0.4 -> atan2(0.4, 0) = pi/2 -> rot = 0.
  //   shift (rot 0): dy = h * shift_y = 0.2 * (-0.5) = -0.1
  //     -> center (0.4, 0.3).
  //   square_long: 0.2 x 0.2 at aspect 1 is already square, no-op.
  //   scale 2.6: w = h = 0.2 * 2.6 = 0.52.
  EXPECT_NEAR(palm_roi.center.x, 0.4f, 1e-4f);
  EXPECT_NEAR(palm_roi.center.y, 0.3f, 1e-4f);
  EXPECT_NEAR(palm_roi.w, 0.52f, 1e-4f);
  EXPECT_NEAR(palm_roi.h, 0.52f, 1e-4f);
  EXPECT_NEAR(palm_roi.rotation_rad, 0.0f, 1e-4f);

  // Landmark path: the 12-point subset spans x in [0.3, 0.5] and
  // y in [0.3, 0.7] (lm[0] at y = 0.7 is the lowest subset point) ->
  //   box 0.2 x 0.4, center (0.4, 0.5).
  //   rotation: origin lm[0] (0.5, 0.7), weighted target (0.5, 0.3)
  //     (all three MCPs coincide) -> dx = 0, dy = 0.4 -> rot = 0.
  //   shift (rot 0): dy = h * shift_y = 0.4 * (-0.1) = -0.04
  //     -> center (0.4, 0.46).
  //   square_long: w_px = 0.2, h_px = 0.4 -> h is longer,
  //     so w grows to h / aspect = 0.4 -> 0.4 x 0.4.
  //   scale 2.0: w = h = 0.4 * 2.0 = 0.8.
  EXPECT_NEAR(lm_roi.center.x, 0.4f, 1e-4f);
  EXPECT_NEAR(lm_roi.center.y, 0.46f, 1e-4f);
  EXPECT_NEAR(lm_roi.w, 0.8f, 1e-4f);
  EXPECT_NEAR(lm_roi.h, 0.8f, 1e-4f);
  EXPECT_NEAR(lm_roi.rotation_rad, 0.0f, 1e-4f);

  // And they genuinely differ, which is the regression this test is named
  // for.
  EXPECT_NE(palm_roi.w, lm_roi.w);
  EXPECT_NE(palm_roi.h, lm_roi.h);
  EXPECT_NE(palm_roi.center.y, lm_roi.center.y);
}

TEST(Roi, NestedWeightingIsNotFlatThirds) {
  // Index MCP (lm[5]) and ring MCP (lm[13]) sit at y = 0.0; middle MCP
  // (lm[9]) sits at y = 0.9. Flat-thirds average: (0 + 0.9 + 0) / 3 = 0.3.
  // Nested 0.25/0.5/0.25 average: 0.25*0 + 0.5*0.9 + 0.25*0 = 0.45.
  // Wrist (lm[0]) at (0.5, 1.0), dx == 0 in both cases, so the two
  // weightings give different dy and therefore different (nonzero)
  // rotations -- distinguishing them requires exactly this kind of
  // asymmetric placement among the three targets.
  // Use wrist at (0.5, 1.0); index/ring MCP at (0.3, 0.0); middle MCP at
  // (0.3, 0.9), so all three targets share x = 0.3 (dx is identical either
  // way) but y differs by weighting:
  //   flat thirds:  x1 = 0.3, y1 = (0+0.9+0)/3 = 0.3
  //       dx = 0.3 - 0.5 = -0.2, dy = -(0.3 - 1.0) = 0.7
  //       atan2(0.7, -0.2) = 1.849096 rad
  //       rotation = pi/2 - 1.849096 = -0.278300 rad
  //   nested 0.25/0.5/0.25: x1 = 0.3, y1 = ((0+0)/2 + 0.9)/2 = 0.45
  //       dx = -0.2, dy = -(0.45 - 1.0) = 0.55
  //       atan2(0.55, -0.2) = 1.919478 rad
  //       rotation = pi/2 - 1.919478 = -0.348771 rad
  // The two rotations (-0.2783 vs -0.3488) differ by ~0.070 rad, well above
  // any float tolerance, so a flat-thirds implementation fails this test.
  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.5f});
  lm[0] = Point2{0.5f, 1.0f};
  lm[5] = Point2{0.3f, 0.0f};
  lm[13] = Point2{0.3f, 0.0f};
  lm[9] = Point2{0.3f, 0.9f};
  lm[1] = Point2{0.0f, 0.0f};
  lm[2] = Point2{0.6f, 0.0f};
  lm[3] = Point2{0.0f, 1.0f};
  lm[6] = Point2{0.6f, 1.0f};
  lm[10] = Point2{0.0f, 0.5f};
  lm[14] = Point2{0.6f, 0.5f};
  lm[17] = Point2{0.1f, 0.5f};
  lm[18] = Point2{0.5f, 0.5f};

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const RotatedRect roi = RoiFromLandmarks(lm, /*aspect=*/1.0f, o);

  const float expected_nested_rotation = -0.348771f;
  const float expected_flat_rotation = -0.278300f;
  EXPECT_NEAR(roi.rotation_rad, expected_nested_rotation, 1e-3f);
  EXPECT_GT(std::fabs(roi.rotation_rad - expected_flat_rotation), 0.01f);
}

TEST(Roi, BoundingBoxExcludesFingertips) {
  // Place every one of the 5 fingertip indices (4, 8, 12, 16, 20) far
  // outside the 12-point subset's extent. If the implementation (wrongly)
  // uses all 21 landmarks for the bbox, w/h grow far past the
  // scale-2.0-of-0.2 == 0.4 expected here. The 12-point subset (plus wrist
  // rotation setup) is the same as ExpansionMatchesConfiguredScale.
  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.3f});
  lm[0] = Point2{0.4f, 0.4f};
  lm[5] = Point2{0.4f, 0.2f};
  lm[9] = Point2{0.4f, 0.2f};
  lm[13] = Point2{0.4f, 0.2f};
  lm[1] = Point2{0.3f, 0.2f};
  lm[2] = Point2{0.5f, 0.2f};
  lm[3] = Point2{0.3f, 0.4f};
  lm[6] = Point2{0.5f, 0.4f};
  lm[10] = Point2{0.3f, 0.3f};
  lm[14] = Point2{0.5f, 0.3f};
  lm[17] = Point2{0.35f, 0.3f};
  lm[18] = Point2{0.45f, 0.3f};
  // Fingertips placed far outside [0.3, 0.5] x [0.2, 0.4].
  lm[4] = Point2{-5.0f, -5.0f};
  lm[8] = Point2{5.0f, -5.0f};
  lm[12] = Point2{-5.0f, 5.0f};
  lm[16] = Point2{5.0f, 5.0f};
  lm[20] = Point2{10.0f, 10.0f};

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const RotatedRect roi = RoiFromLandmarks(lm, /*aspect=*/1.0f, o);

  // Same expectation as ExpansionMatchesConfiguredScale: w == h == 0.4,
  // center (0.4, 0.28). A fingertip-inclusive bbox would blow this up to
  // roughly 10-20x, so 1e-3 tolerance is discriminating.
  EXPECT_NEAR(roi.w, 0.4f, 1e-3f);
  EXPECT_NEAR(roi.h, 0.4f, 1e-3f);
  EXPECT_NEAR(roi.center.x, 0.4f, 1e-3f);
  EXPECT_NEAR(roi.center.y, 0.28f, 1e-3f);
}

TEST(Roi, AspectFactorChangesRotation) {
  // Same keypoint geometry with a nonzero dx, evaluated at aspect == 1.0
  // and aspect == 16/9, must give different rotations. An implementation
  // that omits the aspect multiplier on the x term would pass this at
  // aspect == 1 but give the same (wrong) answer at 16/9 as it did at 1.0.
  //
  //   kp0 = (0.5, 0.6), kp2 = (0.3, 0.4) (wrist below-right of MCP)
  //   dx_norm = x1 - x0 = 0.3 - 0.5 = -0.2
  //   dy = -(y1 - y0) = -(0.4 - 0.6) = 0.2
  //   aspect = 1.0: atan2(0.2, -0.2*1.0) = atan2(0.2,-0.2) = 2.356194 rad
  //       rotation = pi/2 - 2.356194 = -0.785398 rad (== -pi/4)
  //   aspect = 16/9 = 1.777778: atan2(0.2, -0.2*1.777778)
  //       = atan2(0.2, -0.355556) = 2.629011 rad
  //       rotation = pi/2 - 2.629011 = -1.058407 rad
  PalmDetection d{};
  d.box = Rect{0.2f, 0.3f, 0.3f, 0.3f};
  d.keypoints[0] = Point2{0.5f, 0.6f};
  d.keypoints[2] = Point2{0.3f, 0.4f};
  d.score = 1.0f;

  const PalmRoiOptions o = DefaultPalmRoiOptions();
  const RotatedRect roi_square = RoiFromPalm(d, 1.0f, o);
  const RotatedRect roi_wide = RoiFromPalm(d, 16.0f / 9.0f, o);

  EXPECT_NEAR(roi_square.rotation_rad, -kPi / 4.0f, 1e-3f);
  EXPECT_NEAR(roi_wide.rotation_rad, -1.058407f, 1e-3f);
  EXPECT_GT(std::fabs(roi_square.rotation_rad - roi_wide.rotation_rad), 0.1f);
}

TEST(Roi, RoiFromPalmUsesDetectionBoxNotKeypointHull) {
  // The detection's box and its keypoints cover deliberately different
  // extents. RoiFromPalm must take its rect from d.box (per ruling T7-E);
  // the keypoints only set rotation. With square_long + scale 2.6 applied
  // to the box (0.1 x 0.1, already square at aspect 1):
  //   shift: rotation == 0 here (keypoints stacked vertically, dx == 0,
  //     wrist below MCP) -> dy = h * shift_y = 0.1 * -0.5 = -0.05
  //     box center = (0.45, 0.45) -> shifted center = (0.45, 0.4)
  //   square_long: already square, no-op.
  //   scale: w = h = 0.1 * 2.6 = 0.26
  // A keypoint-hull implementation would instead see keypoints spanning a
  // tiny 0.0-extent vertical segment at x == 0.9, giving a center near
  // x == 0.9 and a degenerate (or very different) box -- far from x == 0.45.
  PalmDetection d{};
  d.box = Rect{0.4f, 0.4f, 0.1f, 0.1f};  // center (0.45, 0.45)
  d.keypoints[0] = Point2{0.9f, 0.9f};   // wrist, far from the box
  d.keypoints[2] = Point2{0.9f, 0.7f};   // MCP, same x as wrist -> dx == 0
  d.score = 1.0f;

  const PalmRoiOptions o = DefaultPalmRoiOptions();
  const RotatedRect roi = RoiFromPalm(d, /*aspect=*/1.0f, o);

  EXPECT_NEAR(roi.center.x, 0.45f, 1e-3f);
  EXPECT_NEAR(roi.center.y, 0.4f, 1e-3f);
  EXPECT_NEAR(roi.w, 0.26f, 1e-3f);
  EXPECT_NEAR(roi.h, 0.26f, 1e-3f);
}

TEST(Roi, LandmarkAspectFactorChangesRotation) {
  // Landmark-path twin of AspectFactorChangesRotation. Both paths currently
  // share one ComputeRotation helper, so the palm-only test happens to
  // cover this -- but that is an implementation coincidence, not a pinned
  // contract. If the helper is ever duplicated or inlined, the landmark
  // path could silently lose the aspect factor while the suite stayed
  // green. Aspect-dependent rotation is invisible on a square test frame
  // and wrong on every real camera, so pin it on this path directly.
  //
  //   origin lm[0] = (0.5, 0.6); all three rotation targets at (0.3, 0.4),
  //   so the weighted average is (0.3, 0.4) whatever the weights.
  //   dx_norm = 0.3 - 0.5 = -0.2,  dy = -(0.4 - 0.6) = 0.2
  //   aspect = 1.0:    atan2(0.2, -0.2)       = 2.356194 rad
  //       rotation = pi/2 - 2.356194 = -0.785398 rad (== -pi/4)
  //   aspect = 16/9:   atan2(0.2, -0.355556)  = 2.629011 rad
  //       rotation = pi/2 - 2.629011 = -1.058407 rad
  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.4f, 0.5f});
  lm[0] = Point2{0.5f, 0.6f};
  lm[5] = Point2{0.3f, 0.4f};
  lm[9] = Point2{0.3f, 0.4f};
  lm[13] = Point2{0.3f, 0.4f};
  // A non-degenerate bbox subset, so the rect itself is well-formed.
  lm[1] = Point2{0.25f, 0.35f};
  lm[2] = Point2{0.55f, 0.35f};
  lm[3] = Point2{0.25f, 0.65f};
  lm[6] = Point2{0.55f, 0.65f};
  lm[10] = Point2{0.3f, 0.5f};
  lm[14] = Point2{0.5f, 0.5f};
  lm[17] = Point2{0.35f, 0.5f};
  lm[18] = Point2{0.45f, 0.5f};

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const RotatedRect roi_square = RoiFromLandmarks(lm, 1.0f, o);
  const RotatedRect roi_wide = RoiFromLandmarks(lm, 16.0f / 9.0f, o);

  EXPECT_NEAR(roi_square.rotation_rad, -kPi / 4.0f, 1e-3f);
  EXPECT_NEAR(roi_wide.rotation_rad, -1.058407f, 1e-3f);
  EXPECT_GT(std::fabs(roi_square.rotation_rad - roi_wide.rotation_rad), 0.1f);
}

TEST(Roi, LandmarkRoiIsSquareInPixelsWhenSquareLongSet) {
  // Landmark-path twin of RoiIsSquareInPixelsWhenSquareLongSet, which is
  // palm-only. DefaultLandmarkRoiOptions sets square_long true and
  // scale_x == scale_y, so after TransformRect's internal squaring and
  // uniform scaling, h / w must equal aspect (i.e. the crop is square in
  // pixels). The 12-point subset below spans x in [0.3, 0.7] and
  // y in [0.45, 0.55] -- deliberately wide and flat, so squaring has real
  // work to do rather than being a no-op.
  std::array<Point2, 21> lm = ParkedLandmarks(Point2{0.5f, 0.5f});
  lm[0] = Point2{0.5f, 0.55f};
  lm[5] = Point2{0.5f, 0.45f};
  lm[9] = Point2{0.5f, 0.45f};
  lm[13] = Point2{0.5f, 0.45f};
  lm[1] = Point2{0.3f, 0.45f};
  lm[2] = Point2{0.7f, 0.45f};
  lm[3] = Point2{0.3f, 0.55f};
  lm[6] = Point2{0.7f, 0.55f};
  lm[10] = Point2{0.4f, 0.5f};
  lm[14] = Point2{0.6f, 0.5f};
  lm[17] = Point2{0.45f, 0.5f};
  lm[18] = Point2{0.55f, 0.5f};

  const LandmarkRoiOptions o = DefaultLandmarkRoiOptions();
  const float aspect = 16.0f / 9.0f;
  const RotatedRect roi = RoiFromLandmarks(lm, aspect, o);
  EXPECT_NEAR(roi.h / roi.w, aspect, 1e-4f);
}
