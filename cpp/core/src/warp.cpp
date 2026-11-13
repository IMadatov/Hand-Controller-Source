#include "hc/warp.hpp"

#include <cmath>

namespace hc {

namespace {

// The four texel addresses and the bilinear weights for one sample position.
// Computed once per output pixel and reused for all three channels: the tap
// geometry depends only on (px, py), not on the channel, so recomputing it
// per channel would triple the floor/weight work in the hottest loop.
//
// A null pointer marks a texel outside [0, width) x [0, height). Such a
// texel contributes the value 0 to the blend (MediaPipe's BORDER_ZERO) while
// its in-range neighbours still contribute normally -- the zeroing is
// per-sample, never per output pixel.
struct BilinearTaps {
  const std::uint8_t* p00;
  const std::uint8_t* p10;
  const std::uint8_t* p01;
  const std::uint8_t* p11;
  float wx0, wx1, wy0, wy1;
};

// Computes the tap addresses and weights for pixel coordinates (px, py),
// where (0,0) is the top-left corner of pixel (0,0) and pixel centers sit at
// half-integer coordinates (matching the convention used to build the crop
// coordinates below).
BilinearTaps ComputeTaps(const ImageView& src, float px, float py) {
  // Texel (ix, iy) covers [ix, ix+1) x [iy, iy+1), so its center is at
  // (ix+0.5, iy+0.5). Convert from pixel coordinates to texel-center-relative
  // coordinates before taking floor, so that bilinear weights come out
  // correctly.
  const float fx = px - 0.5f;
  const float fy = py - 0.5f;

  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  BilinearTaps t;
  t.wx1 = fx - static_cast<float>(x0);
  t.wy1 = fy - static_cast<float>(y0);
  t.wx0 = 1.0f - t.wx1;
  t.wy0 = 1.0f - t.wy1;

  // Address of texel (ix, iy)'s first channel byte, or nullptr if the texel
  // lies outside the source. Row addressing always goes through `stride`,
  // which may exceed width * 3.
  auto Addr = [&](int ix, int iy) -> const std::uint8_t* {
    if (ix < 0 || ix >= src.width || iy < 0 || iy >= src.height) {
      return nullptr;
    }
    return src.data + static_cast<std::size_t>(iy) * src.stride + ix * 3;
  };

  t.p00 = Addr(x0, y0);
  t.p10 = Addr(x1, y0);
  t.p01 = Addr(x0, y1);
  t.p11 = Addr(x1, y1);
  return t;
}

// Blends one channel through the precomputed taps. The arithmetic order is
// identical to computing the taps inline, so results are bit-for-bit the
// same as the per-channel formulation.
inline float BlendChannel(const BilinearTaps& t, int channel) {
  const float v00 = t.p00 ? static_cast<float>(t.p00[channel]) : 0.0f;
  const float v10 = t.p10 ? static_cast<float>(t.p10[channel]) : 0.0f;
  const float v01 = t.p01 ? static_cast<float>(t.p01[channel]) : 0.0f;
  const float v11 = t.p11 ? static_cast<float>(t.p11[channel]) : 0.0f;

  const float top = v00 * t.wx0 + v10 * t.wx1;
  const float bottom = v01 * t.wx0 + v11 * t.wx1;
  return top * t.wy0 + bottom * t.wy1;
}

// Destination layout selector for WarpSample below. The two public entry
// points differ ONLY in how they turn (pixel index, channel slot) into a
// destination offset, so the sampling loop is written once and the layout is
// resolved at compile time via `if constexpr` -- no branch in the inner loop,
// and no chance of the two layouts drifting apart.
enum class TensorLayout { kNchw, kNhwc };

template <TensorLayout kLayout>
void WarpImpl(const ImageView& src, const RotatedRect& roi,
              const WarpOptions& o, float* dst) {
  const int side = o.side;
  const float W = static_cast<float>(src.width);
  const float H = static_cast<float>(src.height);

  const float cos_r = std::cos(roi.rotation_rad);
  const float sin_r = std::sin(roi.rotation_rad);

  const float cx_px = roi.center.x * W;
  const float cy_px = roi.center.y * H;

  // Source-format channel indices for (logical R, G, B).
  // Rgb8 storage: byte0=R, byte1=G, byte2=B.
  // Bgr8 storage: byte0=B, byte1=G, byte2=R.
  const int src_r = (src.format == PixelFormat::Rgb8) ? 0 : 2;
  const int src_g = 1;
  const int src_b = (src.format == PixelFormat::Rgb8) ? 2 : 0;

  // Destination channel slot order depends on o.output_rgb. For NCHW a slot
  // is a plane index; for NHWC it is the offset within each pixel's triple.
  const int slot_for_r = o.output_rgb ? 0 : 2;
  const int slot_for_g = 1;
  const int slot_for_b = o.output_rgb ? 2 : 0;

  const int plane_size = side * side;

  // Destination offset for channel slot `slot` of output pixel `idx`.
  auto DstIndex = [plane_size](int slot, int idx) {
    if constexpr (kLayout == TensorLayout::kNchw) {
      return slot * plane_size + idx;
    } else {
      return idx * 3 + slot;
    }
  };

  for (int oy = 0; oy < side; ++oy) {
    // Crop-space normalized coordinate of this output row's center.
    const float v = (static_cast<float>(oy) + 0.5f) / static_cast<float>(side);
    const float off_y = v - 0.5f;
    for (int ox = 0; ox < side; ++ox) {
      const float u = (static_cast<float>(ox) + 0.5f) / static_cast<float>(side);
      const float off_x = u - 0.5f;

      // Offset in pixels (source frame units) before rotation.
      const float dx_px = off_x * roi.w * W;
      const float dy_px = off_y * roi.h * H;

      // Rotate by roi.rotation_rad in pixel space.
      const float rx = dx_px * cos_r - dy_px * sin_r;
      const float ry = dx_px * sin_r + dy_px * cos_r;

      const float src_px = cx_px + rx;
      const float src_py = cy_px + ry;

      // Tap positions and weights depend only on the sample position, so
      // compute them once and fetch all three channels from the same four
      // texel addresses.
      const BilinearTaps taps = ComputeTaps(src, src_px, src_py);

      const float r = BlendChannel(taps, src_r);
      const float g = BlendChannel(taps, src_g);
      const float b = BlendChannel(taps, src_b);

      const int idx = oy * side + ox;
      dst[DstIndex(slot_for_r, idx)] = r * o.scale + o.bias;
      dst[DstIndex(slot_for_g, idx)] = g * o.scale + o.bias;
      dst[DstIndex(slot_for_b, idx)] = b * o.scale + o.bias;
    }
  }
}

}  // namespace

void WarpToTensorNchw(const ImageView& src, const RotatedRect& roi,
                      const WarpOptions& o, float* dst) {
  WarpImpl<TensorLayout::kNchw>(src, roi, o, dst);
}

void WarpToTensorNhwc(const ImageView& src, const RotatedRect& roi,
                      const WarpOptions& o, float* dst) {
  WarpImpl<TensorLayout::kNhwc>(src, roi, o, dst);
}

Point2 CropToFrame(const RotatedRect& roi, float aspect, Point2 p_crop) {
  // aspect = W / H. Work in "height units" (W = aspect, H = 1) so we never
  // need the actual pixel dimensions, matching TransformRect's convention.
  const float off_x = p_crop.x - 0.5f;
  const float off_y = p_crop.y - 0.5f;

  const float dx_px = off_x * roi.w * aspect;
  const float dy_px = off_y * roi.h;

  const float cos_r = std::cos(roi.rotation_rad);
  const float sin_r = std::sin(roi.rotation_rad);

  const float rx = dx_px * cos_r - dy_px * sin_r;
  const float ry = dx_px * sin_r + dy_px * cos_r;

  Point2 out;
  out.x = roi.center.x + rx / aspect;
  out.y = roi.center.y + ry;
  return out;
}

}  // namespace hc
