#include "opencv_frame_source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "hc/frame_source.hpp"
#include "hc/image_view.hpp"

using hc::CameraOptions;
using hc::Frame;
using hc::OpenCvFrameSource;
using hc::PixelFormat;

namespace {

// Where generated videos go. CMake points HC_TEST_TMP_DIR at a directory
// under the build tree, so these tests never write into the source tree and
// never collide with another build's files.
std::string TempPath(const std::string& name) {
  return std::string(HC_TEST_TMP_DIR) + "/" + name;
}

// Writes a short MJPG-in-AVI video and returns its path, or an empty string if
// the writer could not be opened.
//
// MJPG and .avi specifically: OpenCV carries a built-in Motion JPEG encoder
// that needs no external codec, so this works on a machine with no FFmpeg and
// no GStreamer -- which is exactly this machine. Using .mp4 here would make
// the test depend on a codec that is not installed.
std::string WriteTestVideo(const std::string& name, int frames, int width, int height) {
  const std::string path = TempPath(name);
  cv::VideoWriter writer(path, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30.0,
                         cv::Size(width, height));
  if (!writer.isOpened()) return std::string();

  for (int i = 0; i < frames; ++i) {
    // A per-frame-distinct fill, so a source that returned the same frame
    // twice would be visible to a test that cared to look.
    cv::Mat bgr(height, width, CV_8UC3,
                cv::Scalar(10 + i * 5, 40 + i * 3, 90 + i * 2));
    writer.write(bgr);
  }
  writer.release();
  return path;
}

}  // namespace

TEST(OpenCvFrameSource, OpenFileOnMissingPathReturnsNull) {
  EXPECT_EQ(OpenCvFrameSource::OpenFile(TempPath("does-not-exist.avi")), nullptr);
}

TEST(OpenCvFrameSource, OpenCameraOnAbsentDeviceReturnsNull) {
  // There is no camera on this machine, so this is the behavior that actually
  // runs here: a clean nullptr, not a throw and not a hang.
  CameraOptions o;
  o.device_index = 99;
  EXPECT_EQ(OpenCvFrameSource::OpenCamera(o), nullptr);
}

TEST(OpenCvFrameSource, ReadsEveryFrameOfAGeneratedVideo) {
  const int kFrames = 7;
  const std::string path = WriteTestVideo("read_all.avi", kFrames, 160, 120);
  ASSERT_FALSE(path.empty()) << "cv::VideoWriter could not open an MJPG .avi";

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  int count = 0;
  Frame f{};
  while (source->Next(f)) {
    EXPECT_EQ(f.view.width, 160);
    EXPECT_EQ(f.view.height, 120);
    EXPECT_NE(f.view.data, nullptr);
    // Not stride == width * 3: decoded Mats may pad their rows, and the whole
    // point of carrying a stride is that nothing downstream assumes packed.
    EXPECT_GE(f.view.stride, f.view.width * 3);
    ++count;
  }
  EXPECT_EQ(count, kFrames);

  // Terminal once ended: the IFrameSource contract says the first false is
  // forever.
  EXPECT_FALSE(source->Next(f));
}

TEST(OpenCvFrameSource, ReportsFormatForAFileSource) {
  const std::string path = WriteTestVideo("describe.avi", 2, 160, 120);
  ASSERT_FALSE(path.empty());

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  const std::string desc = source->DescribeFormat();
  EXPECT_FALSE(desc.empty());
  EXPECT_NE(desc.find("160"), std::string::npos) << "description: " << desc;
  EXPECT_NE(desc.find("120"), std::string::npos) << "description: " << desc;
}

TEST(OpenCvFrameSource, ViewFormatIsBgrForOpenCvFrames) {
  // OpenCV decodes to BGR; the view must say so rather than claim Rgb8. A
  // wrong format here is spec defect 6 reintroduced -- the Python version
  // ran a full-frame RGB2BGR pass over a frame that was already BGR, which
  // both swapped red and blue and cost a pointless copy per frame.
  const std::string path = WriteTestVideo("format.avi", 1, 64, 48);
  ASSERT_FALSE(path.empty());

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  Frame f{};
  ASSERT_TRUE(source->Next(f));
  EXPECT_EQ(f.view.format, PixelFormat::Bgr8);
}

TEST(OpenCvFrameSource, IndexesFramesFromZeroAndIncrementsByOne) {
  // The Frame::index contract: the producing source's own counter, starting
  // at 0 and stepping by exactly one. LatestFrameSource relies on this to
  // make its dropped frames observable as gaps.
  const std::string path = WriteTestVideo("index.avi", 4, 64, 48);
  ASSERT_FALSE(path.empty());

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  std::vector<std::uint64_t> indices;
  Frame f{};
  while (source->Next(f)) indices.push_back(f.index);

  ASSERT_EQ(indices.size(), 4u);
  for (std::size_t i = 0; i < indices.size(); ++i) {
    EXPECT_EQ(indices[i], static_cast<std::uint64_t>(i));
  }
}

TEST(OpenCvFrameSource, CloseIsIdempotentAndEndsTheSource) {
  const std::string path = WriteTestVideo("close.avi", 5, 64, 48);
  ASSERT_FALSE(path.empty());

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  Frame f{};
  ASSERT_TRUE(source->Next(f));

  source->Close();
  EXPECT_FALSE(source->Next(f));
  source->Close();  // Idempotent: a second call must be safe and change nothing.
  EXPECT_FALSE(source->Next(f));
}

TEST(OpenCvFrameSource, TimestampsAreMonotonicNonDecreasing) {
  const std::string path = WriteTestVideo("stamps.avi", 4, 64, 48);
  ASSERT_FALSE(path.empty());

  auto source = OpenCvFrameSource::OpenFile(path);
  ASSERT_NE(source, nullptr);

  Frame f{};
  std::int64_t previous = -1;
  while (source->Next(f)) {
    EXPECT_GE(f.timestamp_us, previous);
    previous = f.timestamp_us;
  }
  EXPECT_GT(previous, 0);
}
