#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core/mat.hpp>
#include <opencv2/videoio.hpp>

#include "hc/frame_source.hpp"

namespace hc {

// How to open a camera. The defaults are the spec's: 1280x720 and MJPEG,
// because negotiating MJPEG instead of the driver's default YUYV is named
// there as the single largest real throughput lever in the project -- an
// uncompressed 720p YUYV stream saturates USB bandwidth and caps the camera
// at a low frame rate before any of our code runs.
struct CameraOptions {
  int device_index = 0;
  int width = 1280;
  int height = 720;
  bool prefer_mjpeg = true;
};

// An IFrameSource backed by OpenCV's VideoCapture: either a V4L2 camera or a
// video file.
//
// This class lives in the app layer, never the core: it is the boundary where
// OpenCV's cv::Mat becomes a plain ImageView, so that nothing downstream of it
// has an OpenCV type in its signature.
//
// It performs NO colour conversion. OpenCV decodes to BGR, so the view it
// hands out is PixelFormat::Bgr8 and the bytes are the decoder's own. The
// Python version's wrong-and-wasteful RGB2BGR pass over already-BGR pixels
// (spec defect 6) is not reintroduced here.
class OpenCvFrameSource : public IFrameSource {
 public:
  // Both factories return nullptr on failure rather than throwing: a missing
  // file or an absent camera is an ordinary, expected outcome that the caller
  // has to handle either way, not an exceptional one.
  static std::unique_ptr<OpenCvFrameSource> OpenCamera(const CameraOptions& options);
  static std::unique_ptr<OpenCvFrameSource> OpenFile(const std::string& path);

  ~OpenCvFrameSource() override;

  OpenCvFrameSource(const OpenCvFrameSource&) = delete;
  OpenCvFrameSource& operator=(const OpenCvFrameSource&) = delete;

  // See IFrameSource for the lifetime contract: the returned view points into
  // this source's own decode buffer and is valid only until the next Next()
  // or Close() call.
  bool Next(Frame& out) override;
  void Close() override;

  // What was actually negotiated, not what was asked for -- the fourcc and
  // the resolution read back off the capture after it opened. A camera that
  // silently refused MJPEG, or that gave a different resolution than
  // requested, is visible here and nowhere else.
  std::string DescribeFormat() const;

 private:
  OpenCvFrameSource();

  cv::VideoCapture capture_;
  cv::Mat frame_;
  std::string format_description_;
  std::uint64_t next_index_ = 0;
  bool ended_ = false;
};

}  // namespace hc
