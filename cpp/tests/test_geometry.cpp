#include "hc/geometry.hpp"

#include <cmath>

#include <gtest/gtest.h>

using hc::BoundingBox;
using hc::IoU;
using hc::Point2;
using hc::Rect;
using hc::RectTransform;
using hc::RotatedRect;
using hc::SquareLong;
using hc::TransformRect;

namespace {
constexpr float kPi = 3.14159265358979323846f;
}  // namespace

TEST(Geometry, BoundingBoxOfThreePoints) {
  const Point2 pts[] = {{0.2f, 0.5f}, {0.4f, 0.1f}, {0.3f, 0.9f}};
  const Rect b = BoundingBox(pts, 3);
  EXPECT_FLOAT_EQ(b.x, 0.2f);
  EXPECT_FLOAT_EQ(b.y, 0.1f);
  EXPECT_FLOAT_EQ(b.w, 0.2f);
  EXPECT_FLOAT_EQ(b.h, 0.8f);
}

TEST(Geometry, IoUOfIdenticalRectsIsOne) {
  const Rect a{0.0f, 0.0f, 0.5f, 0.5f};
  const Rect b{0.0f, 0.0f, 0.5f, 0.5f};
  EXPECT_FLOAT_EQ(IoU(a, b), 1.0f);
}

TEST(Geometry, IoUOfDisjointRectsIsZero) {
  const Rect a{0.0f, 0.0f, 0.2f, 0.2f};
  const Rect b{0.5f, 0.5f, 0.2f, 0.2f};
  EXPECT_FLOAT_EQ(IoU(a, b), 0.0f);
}

TEST(Geometry, IoUOfHalfOverlapIsOneThird) {
  const Rect a{0.0f, 0.0f, 0.2f, 0.2f};
  const Rect b{0.1f, 0.0f, 0.2f, 0.2f};
  EXPECT_NEAR(IoU(a, b), 1.0f / 3.0f, 1e-6f);
}

TEST(Geometry, SquareLongIsSquareInPixelsNotNormalized) {
  // aspect 16/9: a rect 0.2 wide and 0.2 tall is 1.78x wider in pixels than
  // tall, so squaring on the long side must grow h, not w.
  const RotatedRect r =
      SquareLong({{0.5f, 0.5f}, 0.2f, 0.2f, 0.0f}, 16.0f / 9.0f);
  EXPECT_FLOAT_EQ(r.w, 0.2f);
  EXPECT_FLOAT_EQ(r.h, 0.2f * 16.0f / 9.0f);
}

TEST(Geometry, SquareLongGrowsWidthWhenHeightIsLongerInPixels) {
  // aspect 16/9: a rect 0.1 wide is 0.178 "height units" wide in pixels,
  // shorter than its 0.5 height, so squaring must grow w, not h. This
  // exercises the opposite branch from the test above.
  const RotatedRect r =
      SquareLong({{0.5f, 0.5f}, 0.1f, 0.5f, 0.0f}, 16.0f / 9.0f);
  EXPECT_FLOAT_EQ(r.h, 0.5f);
  EXPECT_FLOAT_EQ(r.w, 0.5f * 9.0f / 16.0f);
}

TEST(Geometry, TransformRectShiftsAlongRotatedAxes) {
  // rotation = pi/2 with shift_y = -0.5 moves the center along +x by
  // 0.5 * h: MediaPipe's x_shift reduces to -H*h*(-0.5)*sin(pi/2)/W, which
  // is positive. y is unchanged because cos(pi/2) == 0.
  const RotatedRect in{{0.5f, 0.5f}, 0.2f, 0.3f, kPi / 2.0f};
  const RectTransform t{1.0f, 1.0f, 0.0f, -0.5f, false};
  const RotatedRect r = TransformRect(in, 1.0f, t);
  EXPECT_NEAR(r.center.x, 0.5f + 0.5f * 0.3f, 1e-6f);
  EXPECT_NEAR(r.center.y, 0.5f, 1e-6f);
  // scale 1.0 and no squaring: dimensions pass through.
  EXPECT_FLOAT_EQ(r.w, 0.2f);
  EXPECT_FLOAT_EQ(r.h, 0.3f);
  EXPECT_FLOAT_EQ(r.rotation_rad, kPi / 2.0f);
}

TEST(Geometry, TransformRectShiftUsesPreScaleDimensions) {
  // The shift is a fraction of the *input* w/h, so scaling must not change
  // the center displacement. Palm->ROI uses scale 2.6, so getting this
  // wrong is invisible at scale 1.0 and wrong at every real call site.
  const RotatedRect in{{0.5f, 0.5f}, 0.2f, 0.3f, kPi / 2.0f};
  const RectTransform unscaled{1.0f, 1.0f, 0.0f, -0.5f, false};
  const RectTransform scaled{2.6f, 2.6f, 0.0f, -0.5f, false};

  const RotatedRect a = TransformRect(in, 1.0f, unscaled);
  const RotatedRect b = TransformRect(in, 1.0f, scaled);

  EXPECT_FLOAT_EQ(b.center.x, a.center.x);
  EXPECT_FLOAT_EQ(b.center.y, a.center.y);
  // ...while the dimensions themselves do scale.
  EXPECT_FLOAT_EQ(b.w, 0.2f * 2.6f);
  EXPECT_FLOAT_EQ(b.h, 0.3f * 2.6f);
}

TEST(Geometry, TransformRectSquareLongPrecedesScale) {
  // Non-square input, square_long on, and scale_x != scale_y. Squaring
  // first gives w = h = 0.2, then scaling gives (0.4, 0.6). Scaling first
  // would give (0.4, 0.3), and squaring that would give (0.4, 0.4).
  const RotatedRect in{{0.5f, 0.5f}, 0.2f, 0.1f, 0.0f};
  const RectTransform t{2.0f, 3.0f, 0.0f, 0.0f, true};
  const RotatedRect r = TransformRect(in, 1.0f, t);
  EXPECT_FLOAT_EQ(r.w, 0.4f);
  EXPECT_FLOAT_EQ(r.h, 0.6f);
}

TEST(Geometry, TransformRectNonUnitAspectConvertsAcrossAxes) {
  // At rotation = pi/2 a shift_y lands entirely on x, and the cross-axis
  // term carries a 1/aspect factor (from MediaPipe's image_height/
  // image_width). The displacement must therefore shrink as aspect grows.
  const RotatedRect in{{0.5f, 0.5f}, 0.2f, 0.3f, kPi / 2.0f};
  const RectTransform t{1.0f, 1.0f, 0.0f, -0.5f, false};
  const float aspect = 16.0f / 9.0f;
  const RotatedRect r = TransformRect(in, aspect, t);

  const float expected_dx = (0.5f * 0.3f) / aspect;
  EXPECT_NEAR(r.center.x, 0.5f + expected_dx, 1e-6f);
  EXPECT_NEAR(r.center.y, 0.5f, 1e-6f);
  // Sanity: this is genuinely smaller than the aspect == 1 displacement,
  // so the parameter is actually being used.
  EXPECT_LT(expected_dx, 0.5f * 0.3f);
}

TEST(Geometry, TransformRectZeroRotationMatchesPlainShift) {
  // MediaPipe special-cases rotation == 0 to a plain w*shift_x /
  // h*shift_y displacement; the general formula must degenerate to it
  // exactly, with no aspect involvement on either axis.
  const RotatedRect in{{0.5f, 0.5f}, 0.2f, 0.3f, 0.0f};
  const RectTransform t{1.0f, 1.0f, 0.1f, -0.2f, false};
  const RotatedRect r = TransformRect(in, 16.0f / 9.0f, t);
  // Exact comparison is valid here: cos(0) == 1 and sin(0) == 0 exactly,
  // so the cross-axis terms vanish identically.
  EXPECT_FLOAT_EQ(r.center.x, 0.5f + 0.2f * 0.1f);
  EXPECT_FLOAT_EQ(r.center.y, 0.5f + 0.3f * -0.2f);
}
