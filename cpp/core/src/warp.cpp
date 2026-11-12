#include "hc/warp.hpp"

#include <cmath>

namespace hc {

namespace {

// Samples one channel of `src` bilinearly at pixel coordinates (px, py),
// where (0,0) is the top-left corner of pixel (0,0) and pixel centers sit at
// half-integer coordinates (matching the convention used to build the crop
// coordinates below). Texels outside [0, width) x [0, height) contribute a
// value of zero (MediaPipe's BORDER_ZERO), applied per contributing sample
// rather than to the whole output pixel.
float SampleChannelZeroBorder(const ImageView& src, int channel, float px,
                               float py) {
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

  const float wx1 = fx - static_cast<float>(x0);
  const float wy1 = fy - static_cast<float>(y0);
  const float wx0 = 1.0f - wx1;
  const float wy0 = 1.0f - wy1;

  auto Texel = [&](int ix, int iy) -> float {
    if (ix < 0 || ix >= src.width || iy < 0 || iy >= src.height) {
      return 0.0f;
    }
    const std::uint8_t* row = src.data + static_cast<std::size_t>(iy) * src.stride;
    return static_cast<float>(row[ix * 3 + channel]);
  };

  const float v00 = Texel(x0, y0);
  const float v10 = Texel(x1, y0);
  const float v01 = Texel(x0, y1);
  const float v11 = Texel(x1, y1);

  const float top = v00 * wx0 + v10 * wx1;
  const float bottom = v01 * wx0 + v11 * wx1;
  return top * wy0 + bottom * wy1;
}

}  // namespace

void WarpToTensorNchw(const ImageView& src, const RotatedRect& roi,
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

  // Destination plane order depends on o.output_rgb.
  const int plane_for_r = o.output_rgb ? 0 : 2;
  const int plane_for_g = 1;
  const int plane_for_b = o.output_rgb ? 2 : 0;

  const int plane_size = side * side;

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

      const float r = SampleChannelZeroBorder(src, src_r, src_px, src_py);
      const float g = SampleChannelZeroBorder(src, src_g, src_px, src_py);
      const float b = SampleChannelZeroBorder(src, src_b, src_px, src_py);

      const int idx = oy * side + ox;
      dst[plane_for_r * plane_size + idx] = r * o.scale + o.bias;
      dst[plane_for_g * plane_size + idx] = g * o.scale + o.bias;
      dst[plane_for_b * plane_size + idx] = b * o.scale + o.bias;
    }
  }
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
