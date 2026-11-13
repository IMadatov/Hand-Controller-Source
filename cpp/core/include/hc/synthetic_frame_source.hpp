#pragma once

#include <cstdint>
#include <vector>

#include "hc/frame_source.hpp"

namespace hc {

// A camera-free frame source: it renders a moving pattern into its own
// buffer so the whole pipeline can be exercised, measured, and tested with
// no camera, no video file, and no OpenCV. Successive frames always differ.
//
// Pixels are PixelFormat::Rgb8 with a packed stride (width * 3).
//
// Not thread-safe (see IFrameSource). Wrap it in LatestFrameSource to run it
// on its own thread.
class SyntheticFrameSource : public IFrameSource {
 public:
  // `frame_count` < 0 means unlimited; 0 means the source is already ended.
  // `width` and `height` are pixels and must both be positive.
  SyntheticFrameSource(int width, int height, int frame_count);

  // Fills `out` with the next frame of the moving pattern.
  //
  // LIFETIME: as per IFrameSource::Next -- `out.view` points into this
  // source's single internal buffer and is invalidated by the next Next()
  // call, by Close(), or by destruction. Each Next() overwrites the pixels
  // the previous view pointed at.
  bool Next(Frame& out) override;

  void Close() override;

 private:
  int width_;
  int height_;
  int frame_count_;  // < 0 means unlimited.
  std::uint64_t next_index_ = 0;
  bool ended_ = false;
  std::vector<std::uint8_t> buffer_;
};

}  // namespace hc
