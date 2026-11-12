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

  // Absolute anchor: plane 0 must hold logical *red*, not merely "whatever
  // plane 2 holds in the other run". MakeBuffer's logical pixel (x, y) is
  // R = x % 256, G = y % 256, B = 0, and an identity ROI with side ==
  // width == height reproduces the source exactly, so at pixel (5, 3) the
  // logical colour is R = 5, G = 3, B = 0. With scale = 1.0 and bias = 0,
  // plane 0 of the output_rgb=true run must therefore be exactly 5 --
  // distinguishable from both G (3) and B (0). An implementation that
  // ignored src.format would read the Bgr8 byte0 and report B = 0 here.
  const int probe = 3 * kSize + 5;  // (x=5, y=3)
  EXPECT_FLOAT_EQ(dst_rgb[0 * plane + probe], 5.0f);
  EXPECT_FLOAT_EQ(dst_rgb[1 * plane + probe], 3.0f);
  EXPECT_FLOAT_EQ(dst_rgb[2 * plane + probe], 0.0f);

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

TEST(Warp, ChannelOrderForAllFormatAndOutputCombinations) {
  // Pins the full src.format x output_rgb cross-product against known
  // logical values, rather than leaving it to be inferred from a symmetric
  // swap. Task 8 feeds these crops straight to the landmark model, where a
  // transposed channel degrades accuracy silently.
  //
  // A uniform colour makes the assertion independent of sampling position:
  // every bilinear blend returns the same value, so any discrepancy is a
  // channel-order fault and nothing else.
  constexpr int kSize = 4;
  constexpr std::uint8_t kR = 200;
  constexpr std::uint8_t kG = 120;
  constexpr std::uint8_t kB = 60;

  struct Combo {
    PixelFormat format;
    bool output_rgb;
  };
  const Combo combos[] = {
      {PixelFormat::Rgb8, true},
      {PixelFormat::Rgb8, false},
      {PixelFormat::Bgr8, true},
      {PixelFormat::Bgr8, false},
  };

  for (const Combo& c : combos) {
    // Store the uniform logical colour in the source's own byte order.
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(kSize) * kSize * 3,
                                   0);
    for (int i = 0; i < kSize * kSize; ++i) {
      std::uint8_t* px = &buf[static_cast<std::size_t>(i) * 3];
      if (c.format == PixelFormat::Rgb8) {
        px[0] = kR;
        px[1] = kG;
        px[2] = kB;
      } else {
        px[0] = kB;
        px[1] = kG;
        px[2] = kR;
      }
    }
    const ImageView src{buf.data(), kSize, kSize, kSize * 3, c.format};

    const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
    WarpOptions opts;
    opts.side = kSize;
    opts.scale = 1.0f;
    opts.bias = 0.0f;
    opts.output_rgb = c.output_rgb;

    std::vector<float> dst(3 * kSize * kSize, -1.0f);
    WarpToTensorNchw(src, roi, opts, dst.data());

    // Expected plane contents are a function of output_rgb only: the source
    // format is an input-decoding detail that must not leak into the output.
    const float expect_plane0 = c.output_rgb ? static_cast<float>(kR)
                                              : static_cast<float>(kB);
    const float expect_plane1 = static_cast<float>(kG);
    const float expect_plane2 = c.output_rgb ? static_cast<float>(kB)
                                              : static_cast<float>(kR);

    const int plane = kSize * kSize;
    const char* fmt_name =
        (c.format == PixelFormat::Rgb8) ? "Rgb8" : "Bgr8";
    SCOPED_TRACE(testing::Message() << "format=" << fmt_name
                                     << " output_rgb=" << c.output_rgb);

    for (int i = 0; i < plane; ++i) {
      EXPECT_FLOAT_EQ(dst[0 * plane + i], expect_plane0) << "idx " << i;
      EXPECT_FLOAT_EQ(dst[1 * plane + i], expect_plane1) << "idx " << i;
      EXPECT_FLOAT_EQ(dst[2 * plane + i], expect_plane2) << "idx " << i;
    }
  }
}

TEST(Warp, AppliesScaleAndBias) {
  // `value * scale + bias` is otherwise only verified by reading the code:
  // nothing in the suite uses a non-default scale, and bias is zero
  // everywhere. Identity ROI so the sampled value is exactly the source
  // byte, then assert the full formula at one known pixel.
  constexpr int kSize = 8;
  const std::vector<std::uint8_t> buf =
      MakeBuffer(kSize, kSize, kSize * 3, PixelFormat::Rgb8);
  const ImageView src{buf.data(), kSize, kSize, kSize * 3, PixelFormat::Rgb8};

  const RotatedRect roi{{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
  WarpOptions opts;
  opts.side = kSize;
  opts.scale = 2.0f / 255.0f;  // non-default
  opts.bias = -1.0f;           // non-zero
  opts.output_rgb = true;

  std::vector<float> dst(3 * kSize * kSize, 0.0f);
  WarpToTensorNchw(src, roi, opts, dst.data());

  // Logical pixel (x=6, y=4) is R = 6, G = 4, B = 0.
  const int plane = kSize * kSize;
  const int probe = 4 * kSize + 6;
  EXPECT_FLOAT_EQ(dst[0 * plane + probe], 6.0f * opts.scale + opts.bias);
  EXPECT_FLOAT_EQ(dst[1 * plane + probe], 4.0f * opts.scale + opts.bias);
  EXPECT_FLOAT_EQ(dst[2 * plane + probe], 0.0f * opts.scale + opts.bias);

  // The bias must actually be present: a zero-valued channel maps to the
  // bias itself, not to zero.
  EXPECT_FLOAT_EQ(dst[2 * plane + probe], -1.0f);
}
