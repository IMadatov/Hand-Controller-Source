#include "opencv_frame_source.hpp"

#include <cctype>
#include <chrono>
#include <sstream>
#include <utility>

#include <opencv2/videoio.hpp>

namespace hc {
namespace {

std::int64_t SteadyNowUs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

// Renders a CAP_PROP_FOURCC value as its four characters. Backends that do not
// report one return 0, and a file opened through some backends reports bytes
// that are not printable, so anything unprintable is described rather than
// emitted raw -- DescribeFormat's output ends up in logs and in the Task 14
// report.
std::string FourccToString(double fourcc_value) {
  const auto packed = static_cast<unsigned int>(static_cast<int>(fourcc_value));
  if (packed == 0) return "none";

  std::string out;
  for (int i = 0; i < 4; ++i) {
    const auto byte = static_cast<unsigned char>((packed >> (8 * i)) & 0xFFu);
    if (std::isprint(byte) == 0) return "unprintable";
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

// Reads back what the capture actually negotiated. Deliberately not a record
// of what was requested: a camera is free to ignore a fourcc or resolution
// request and silently hand back something else, and that divergence is the
// only thing this string exists to make visible.
std::string DescribeCapture(cv::VideoCapture& capture, const std::string& prefix) {
  const int width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
  const int height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
  const double fps = capture.get(cv::CAP_PROP_FPS);

  std::ostringstream oss;
  oss << prefix << " " << width << "x" << height
      << " fourcc=" << FourccToString(capture.get(cv::CAP_PROP_FOURCC));
  if (fps > 0.0) oss << " fps=" << fps;
  return oss.str();
}

}  // namespace

OpenCvFrameSource::OpenCvFrameSource() = default;

OpenCvFrameSource::~OpenCvFrameSource() { Close(); }

std::unique_ptr<OpenCvFrameSource> OpenCvFrameSource::OpenCamera(
    const CameraOptions& options) {
  // Not make_unique: the constructor is private, and this factory is the only
  // thing allowed to call it.
  std::unique_ptr<OpenCvFrameSource> source(new OpenCvFrameSource());

  if (!source->capture_.open(options.device_index)) return nullptr;
  if (!source->capture_.isOpened()) return nullptr;

  // Order matters for V4L2: the pixel format has to be set before the
  // resolution, because the driver picks the frame sizes it will offer based
  // on the format currently selected. Setting them the other way round is a
  // common way to end up silently back on YUYV.
  //
  // MJPEG is the point of this whole block. An uncompressed 720p YUYV stream
  // is ~1.3 MB per frame and saturates USB 2.0 bandwidth, capping the camera
  // at well under 30 fps before any of our code runs; the spec names this as
  // the largest real throughput lever in the project.
  if (options.prefer_mjpeg) {
    source->capture_.set(cv::CAP_PROP_FOURCC,
                         cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  }
  source->capture_.set(cv::CAP_PROP_FRAME_WIDTH, options.width);
  source->capture_.set(cv::CAP_PROP_FRAME_HEIGHT, options.height);

  // Ask for the shallowest queue the backend will give us. A deeper buffer
  // trades latency for smoothness, which is the wrong trade for a controller:
  // a frame delivered late is a cursor that lags the user's hand. Not every
  // backend honours this, so the result is deliberately ignored.
  source->capture_.set(cv::CAP_PROP_BUFFERSIZE, 1);

  source->format_description_ = DescribeCapture(source->capture_, "camera");
  return source;
}

std::unique_ptr<OpenCvFrameSource> OpenCvFrameSource::OpenFile(
    const std::string& path) {
  std::unique_ptr<OpenCvFrameSource> source(new OpenCvFrameSource());

  if (!source->capture_.open(path)) return nullptr;
  if (!source->capture_.isOpened()) return nullptr;

  source->format_description_ = DescribeCapture(source->capture_, "file");
  return source;
}

bool OpenCvFrameSource::Next(Frame& out) {
  // Terminal once ended, per the IFrameSource contract: the first false is
  // false forever, so a consumer may stop polling on it.
  if (ended_) return false;

  if (!capture_.read(frame_) || frame_.empty()) {
    ended_ = true;
    return false;
  }

  // ImageView describes 8-bit 3-channel interleaved pixels and nothing else.
  // VideoCapture normally decodes to exactly that, but a backend handing back
  // grayscale or 16-bit would otherwise be reinterpreted as BGR8 and produce
  // silent garbage downstream, so end the source instead of guessing.
  if (frame_.type() != CV_8UC3) {
    ended_ = true;
    return false;
  }

  // The boundary: cv::Mat in, plain ImageView out, no copy and no colour
  // conversion. frame_ stays owned by this source, which is what makes the
  // view valid only until the next Next() or Close() call.
  out.view.data = frame_.data;
  out.view.width = frame_.cols;
  out.view.height = frame_.rows;
  // The Mat's real step, not cols * 3: decoded rows are routinely padded, and
  // carrying the true stride is what lets core code avoid assuming packed.
  out.view.stride = static_cast<int>(frame_.step);
  out.view.format = PixelFormat::Bgr8;

  out.timestamp_us = SteadyNowUs();
  out.index = next_index_;
  ++next_index_;
  return true;
}

void OpenCvFrameSource::Close() {
  // Idempotent: release() on an already-released capture is a no-op, and
  // ended_ is already true on the second call.
  if (capture_.isOpened()) capture_.release();
  ended_ = true;
}

std::string OpenCvFrameSource::DescribeFormat() const {
  return format_description_;
}

}  // namespace hc
