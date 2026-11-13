#pragma once

#include "hc/geometry.hpp"
#include "hc/image_view.hpp"

namespace hc {

// Options controlling how the WarpToTensor* functions sample and scale pixel
// values into the destination tensor.
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

// Identical to WarpToTensorNchw in every respect -- same sampling, same
// BORDER_ZERO semantics, same scale/bias, same channel-order selection --
// except that `dst` is laid out interleaved HWC (row, column, channel)
// instead of planar CHW. Still `3 * o.side * o.side` floats.
//
// This exists because both ONNX models in this project take NHWC input
// ([1,192,192,3] palm, [1,224,224,3] landmark, per cpp/docs/models.md).
// Writing NHWC directly avoids a separate transpose pass over the tensor on
// the per-hand, per-frame path; the two functions share one sampler, so the
// only difference between them is the destination index.
void WarpToTensorNhwc(const ImageView& src, const RotatedRect& roi,
                      const WarpOptions& o, float* dst);

// Maps a point in the crop's own normalized 0..1 space (as produced by the
// same rotated sampling the WarpToTensor* functions perform) back to
// frame-normalized
// coordinates. `aspect` is frame width / frame height, the same convention
// used throughout hc/geometry.hpp.
Point2 CropToFrame(const RotatedRect& roi, float aspect, Point2 p_crop);

}  // namespace hc
