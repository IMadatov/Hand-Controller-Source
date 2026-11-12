#include "hc/warp.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using hc::CropToFrame;
using hc::ImageView;
using hc::PixelFormat;
using hc::Point2;
using hc::RotatedRect;
using hc::WarpOptions;
using hc::WarpToTensorNchw;

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Builds a width x height RGB8 (or BGR8, depending on `format`) buffer with
// the given `stride`, where channel-order-agnostic content pixel (x, y) is
// {x % 256, y % 256, 0} in *RGB* terms. If `format` is Bgr8, the bytes are
// stored as {0, y % 256, x % 256} so that interpreting them as BGR8 still
// yields logical R = x%256, G = y%256, B = 0.
std::vector<std::uint8_t> MakeBuffer(int width, int height, int stride,
                                      PixelFormat format) {
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint8_t* px = &buf[static_cast<std::size_t>(y) * stride + x * 3];
      const std::uint8_t r = static_cast<std::uint8_t>(x % 256);
      const std::uint8_t g = static_cast<std::uint8_t>(y % 256);
      const std::uint8_t b = 0;
      if (format == PixelFormat::Rgb8) {
        px[0] = r;
        px[1] = g;
        px[2] = b;
      } else {
        // Bgr8 storage order: byte0=B, byte1=G, byte2=R.
        px[0] = b;
        px[1] = g;
        px[2] = r;
      }
    }
  }
  return buf;
}

}  // namespace

TEST(Warp, IdentityRoiReproducesSource) {
  constexpr int kSize = 8;
  const std::vector<std::uint8_t> buf =
      MakeBuffer(kSize, kSize, kSize * 3, PixelFormat::Rgb8);
  const ImageView src{buf.data(), kSize, kSize, kSize * 3, PixelFormat::Rgb8};

  const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
  WarpOptions opts;
  opts.side = kSize;
  opts.scale = 1.0f;
  opts.bias = 0.0f;
  opts.output_rgb = true;

  std::vector<float> dst(3 * kSize * kSize, -1.0f);
  WarpToTensorNchw(src, roi, opts, dst.data());

  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const float expected_r = static_cast<float>(x % 256);
      const float expected_g = static_cast<float>(y % 256);
      const float expected_b = 0.0f;
      const int idx = y * kSize + x;
      EXPECT_NEAR(dst[0 * kSize * kSize + idx], expected_r, 1e-3f)
          << "x=" << x << " y=" << y;
      EXPECT_NEAR(dst[1 * kSize * kSize + idx], expected_g, 1e-3f)
          << "x=" << x << " y=" << y;
      EXPECT_NEAR(dst[2 * kSize * kSize + idx], expected_b, 1e-3f)
          << "x=" << x << " y=" << y;
    }
  }
}

TEST(Warp, HonorsStridePadding) {
  constexpr int kSize = 8;
  const std::vector<std::uint8_t> packed =
      MakeBuffer(kSize, kSize, 24, PixelFormat::Rgb8);
  const std::vector<std::uint8_t> padded =
      MakeBuffer(kSize, kSize, 40, PixelFormat::Rgb8);

  const ImageView src_packed{packed.data(), kSize, kSize, 24,
                              PixelFormat::Rgb8};
  const ImageView src_padded{padded.data(), kSize, kSize, 40,
                              PixelFormat::Rgb8};

  const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
  WarpOptions opts;
  opts.side = kSize;

  std::vector<float> dst_packed(3 * kSize * kSize, 0.0f);
  std::vector<float> dst_padded(3 * kSize * kSize, 0.0f);
  WarpToTensorNchw(src_packed, roi, opts, dst_packed.data());
  WarpToTensorNchw(src_padded, roi, opts, dst_padded.data());

  ASSERT_EQ(dst_packed.size(), dst_padded.size());
  for (std::size_t i = 0; i < dst_packed.size(); ++i) {
    EXPECT_EQ(dst_packed[i], dst_padded[i]) << "mismatch at index " << i;
  }
}

TEST(Warp, SamplingOutsideSourceIsZero) {
  constexpr int kSize = 8;
  const std::vector<std::uint8_t> buf =
      MakeBuffer(kSize, kSize, kSize * 3, PixelFormat::Rgb8);
  const ImageView src{buf.data(), kSize, kSize, kSize * 3, PixelFormat::Rgb8};

  // Centered at 0.95, 0.95 with w=h=0.4: extends well past the right/bottom
  // edges of the 8x8 source.
  const RotatedRect roi{{0.95f, 0.95f}, 0.4f, 0.4f, 0.0f};
  WarpOptions opts;
  opts.side = 8;
  opts.scale = 1.0f;

  std::vector<float> dst(3 * opts.side * opts.side, -1.0f);
  WarpToTensorNchw(src, roi, opts, dst.data());

  // roi spans normalized [0.75, 1.15] x [0.75, 1.15] i.e. pixels [6, 9.2).
  // Source only has pixels [0, 8). Output crop columns/rows whose sampled
  // source coordinate exceeds 8 (with no in-range neighbor) should be
  // exactly zero. The last couple of output columns/rows are safely beyond
  // the source bounds for all four bilinear taps.
  const int side = opts.side;
  // Last output row/col (side-1) maps to source pixel ~ 6 + (7.5/8)*3.2 ~=
  // 9.0, which is out of range with its right/bottom bilinear neighbor also
  // out of range, so it must be exactly 0.
  for (int c = 0; c < 3; ++c) {
    const int idx = (side - 1) * side + (side - 1);
    EXPECT_EQ(dst[c * side * side + idx], 0.0f);
  }
}

TEST(Warp, QuarterTurnRotationMapsCornersCorrectly) {
  constexpr int kSize = 8;
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(kSize) * kSize * 3,
                                 0);
  // Bright pixel at the top-left corner (0,0).
  buf[0] = 255;
  buf[1] = 255;
  buf[2] = 255;
  const ImageView src{buf.data(), kSize, kSize, kSize * 3, PixelFormat::Rgb8};

  const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, kPi / 2.0f};
  WarpOptions opts;
  opts.side = kSize;
  opts.scale = 1.0f;

  std::vector<float> dst(3 * kSize * kSize, -1.0f);
  WarpToTensorNchw(src, roi, opts, dst.data());

  // Find the brightest output pixel in the red channel plane.
  int best_idx = 0;
  float best_val = -1.0f;
  for (int i = 0; i < kSize * kSize; ++i) {
    if (dst[i] > best_val) {
      best_val = dst[i];
      best_idx = i;
    }
  }
  const int best_x = best_idx % kSize;
  const int best_y = best_idx / kSize;

  // A rotation of +pi/2 (counter-clockwise in math convention, applied in
  // pixel space where y grows downward) maps the source's top-left corner
  // (0,0), i.e. pixel-space offset (-W/2,-H/2) from center, by rotating
  // (x,y) -> (x*cos - y*sin, x*sin + y*cos). With theta = pi/2: (x,y) ->
  // (y, -x). So (-W/2, -H/2) -> (-H/2, W/2), i.e. bottom-left corner of the
  // output.
  EXPECT_LE(best_x, 1);
  EXPECT_GE(best_y, kSize - 2);
}

TEST(Warp, OutputRgbFalseSwapsChannelOrder) {
  constexpr int kSize = 8;
  const std::vector<std::uint8_t> buf =
      MakeBuffer(kSize, kSize, kSize * 3, PixelFormat::Bgr8);
  const ImageView src{buf.data(), kSize, kSize, kSize * 3, PixelFormat::Bgr8};

  const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
  WarpOptions opts_rgb;
  opts_rgb.side = kSize;
  opts_rgb.scale = 1.0f;
  opts_rgb.output_rgb = true;

  WarpOptions opts_bgr = opts_rgb;
  opts_bgr.output_rgb = false;

  std::vector<float> dst_rgb(3 * kSize * kSize, 0.0f);
  std::vector<float> dst_bgr(3 * kSize * kSize, 0.0f);
  WarpToTensorNchw(src, roi, opts_rgb, dst_rgb.data());
  WarpToTensorNchw(src, roi, opts_bgr, dst_bgr.data());

  const int plane = kSize * kSize;
  // Plane 1 (green) is identical regardless of output order.
  for (int i = 0; i < plane; ++i) {
    EXPECT_FLOAT_EQ(dst_rgb[plane + i], dst_bgr[plane + i]);
  }
  // Planes 0 and 2 are swapped between the two outputs.
  bool differs0 = false;
  bool differs2 = false;
  for (int i = 0; i < plane; ++i) {
    if (dst_rgb[0 * plane + i] != dst_bgr[0 * plane + i]) differs0 = true;
    if (dst_rgb[2 * plane + i] != dst_bgr[2 * plane + i]) differs2 = true;
    EXPECT_FLOAT_EQ(dst_rgb[0 * plane + i], dst_bgr[2 * plane + i]);
    EXPECT_FLOAT_EQ(dst_rgb[2 * plane + i], dst_bgr[0 * plane + i]);
  }
  EXPECT_TRUE(differs0);
  EXPECT_TRUE(differs2);
}

TEST(Warp, CropToFrameRoundTripsWarpCenter) {
  // A rotated, non-square roi: the crop center must always map back to the
  // roi's own center, regardless of rotation or aspect.
  const RotatedRect roi{{0.3f, 0.7f}, 0.4f, 0.2f, 0.37f};
  const float aspect = 1.77f;
  const Point2 p = CropToFrame(roi, aspect, {0.5f, 0.5f});
  EXPECT_NEAR(p.x, roi.center.x, 1e-6f);
  EXPECT_NEAR(p.y, roi.center.y, 1e-6f);
}

TEST(Warp, CropToFrameMapsOffCentrePointThroughRotationAndAspect) {
  // roi: center {0.5, 0.5}, w = 0.5, h = 0.25, rotation_rad = pi/2,
  // aspect = 2.0 (e.g. a 640x320 frame). p_crop = {1.0, 0.5} is the
  // right-edge midpoint of the crop.
  //
  // dx_px = (1.0 - 0.5) * 0.5 * 640 = 160, dy_px = 0.
  // Rotating (160, 0) by pi/2 gives (0, 160).
  // Normalizing: (0/640, 160/320) = (0, 0.5).
  // Adding the center: {0.5, 1.0}.
  const RotatedRect roi{{0.5f, 0.5f}, 0.5f, 0.25f, kPi / 2.0f};
  const float aspect = 2.0f;
  const Point2 p = CropToFrame(roi, aspect, {1.0f, 0.5f});
  EXPECT_NEAR(p.x, 0.5f, 1e-5f);
  EXPECT_NEAR(p.y, 1.0f, 1e-5f);
}
