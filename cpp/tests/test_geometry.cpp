#include "hc/geometry.hpp"

#include <cmath>

#include <gtest/gtest.h>

using hc::BoundingBox;
using hc::ExpandAndShift;
using hc::IoU;
using hc::Point2;
using hc::Rect;
using hc::RotatedRect;
using hc::SquareLong;

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
  const RotatedRect r = SquareLong({{0.5f, 0.5f}, 0.2f, 0.2f, 0.0f}, 16.0f / 9.0f);
  EXPECT_FLOAT_EQ(r.w, 0.2f);
  EXPECT_FLOAT_EQ(r.h, 0.2f * 16.0f / 9.0f);
}

TEST(Geometry, ExpandAndShiftScalesAboutCenterWhenShiftIsZero) {
  const RotatedRect r =
      ExpandAndShift({{0.5f, 0.5f}, 0.2f, 0.1f, 0.0f}, 2.0f, 2.0f, 0.0f, 0.0f);
  EXPECT_FLOAT_EQ(r.center.x, 0.5f);
  EXPECT_FLOAT_EQ(r.center.y, 0.5f);
  EXPECT_FLOAT_EQ(r.w, 0.4f);
  EXPECT_FLOAT_EQ(r.h, 0.2f);
}

TEST(Geometry, ExpandAndShiftMovesAlongRotatedAxis) {
  // rotation = pi/2, shift_y = -0.5 must move the center along -x, not -y.
  const float kPi = 3.14159265358979323846f;
  const RotatedRect r = ExpandAndShift({{0.5f, 0.5f}, 0.2f, 0.2f, kPi / 2.0f},
                                        1.0f, 1.0f, 0.0f, -0.5f);
  EXPECT_NEAR(r.center.x, 0.4f, 1e-6f);
  EXPECT_NEAR(r.center.y, 0.5f, 1e-6f);
}
