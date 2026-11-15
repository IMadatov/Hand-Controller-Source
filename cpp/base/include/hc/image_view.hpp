#pragma once

#include <cstdint>

namespace hc {

// Pixel layout of the bytes an ImageView points at. The core only ever reads
// 8-bit, 3-channel interleaved pixels; anything else (e.g. YUYV from a camera)
// must be converted before it reaches core code.
enum class PixelFormat { Rgb8, Bgr8 };

// A non-owning view over a rectangular region of pixel memory. ImageView
// never allocates, copies, or frees `data`; the caller owns the buffer and
// must keep it alive for as long as the view is used.
//
// `stride` is the number of bytes between the start of one row and the start
// of the next, and may exceed `width * 3`: V4L2 and MJPEG decode routinely
// produce rows padded beyond the packed width, so nothing in core may assume
// `stride == width * channels`.
struct ImageView {
  const std::uint8_t* data;
  int width;
  int height;
  int stride;
  PixelFormat format;
};

}  // namespace hc
