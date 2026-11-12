#pragma once

#include "hc/geometry.hpp"
#include "hc/image_view.hpp"

namespace hc {

// Options controlling how WarpToTensorNchw samples and scales pixel values
// into the destination tensor.
struct WarpOptions {
  int side;
  float scale = 1.0f / 255.0f;
  float bias = 0.0f;
  bool output_rgb = true;
};

// Samples a rotated region of `src` (described by `roi`, normalized to the
// frame as per RotatedRect's convention: `roi.w` against `src.width` and
// `roi.h` against `src.height`) into `dst`, a caller-owned buffer of
// `3 * o.side * o.side` floats laid out planar CHW (channel, row, column).
//
// Sampling is bilinear. Source texels outside `[0, src.width) x
// [0, src.height)` are treated as zero per-sample (MediaPipe's BORDER_ZERO):
// a bilinear sample with some in-range and some out-of-range contributing
// texels blends the out-of-range ones as 0, rather than zeroing the whole
// output pixel. Each sampled value is scaled by `o.scale` and offset by
// `o.bias` before being written.
//
// `o.output_rgb` selects the channel order of the output tensor's planes;
// it is independent of `src.format`, which describes the *input* layout.
// Row addressing always goes through `src.stride`, which may exceed
// `src.width * 3`.
void WarpToTensorNchw(const ImageView& src, const RotatedRect& roi,
                      const WarpOptions& o, float* dst);

// Maps a point in the crop's own normalized 0..1 space (as produced by the
// same rotated sampling WarpToTensorNchw performs) back to frame-normalized
// coordinates. `aspect` is frame width / frame height, the same convention
// used throughout hc/geometry.hpp.
Point2 CropToFrame(const RotatedRect& roi, float aspect, Point2 p_crop);

}  // namespace hc
