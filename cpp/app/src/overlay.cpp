#include "overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace hc {
namespace {

// MediaPipe's 21-landmark hand skeleton: the bones, as index pairs into
// HandObservation::landmarks. Four per finger plus the three palm links that
// tie the finger bases together, which is what makes the drawing read as a
// hand rather than five loose spiders.
constexpr std::pair<int, int> kConnections[] = {
    // Thumb.
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    // Index.
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    // Middle.
    {9, 10}, {10, 11}, {11, 12},
    // Ring.
    {13, 14}, {14, 15}, {15, 16},
    // Pinky.
    {0, 17}, {17, 18}, {18, 19}, {19, 20},
    // Palm.
    {5, 9}, {9, 13}, {13, 17},
};

// A plain triple rather than cv::Vec3b: Vec has no constexpr constructor, so
// a table of them could not be a compile-time constant.
struct Bgr {
  int b, g, r;
};

// One colour per hand. Track ids come from the tracker's monotonic counter
// and grow without bound, so the lookup wraps; with two hands on screen at a
// time, two adjacent ids only ever collide after a wrap, and even then the
// entries are far enough apart in hue to stay distinguishable.
constexpr Bgr kTrackColors[] = {
    {255, 128, 0},    // orange
    {0, 220, 255},    // yellow
    {0, 200, 0},      // green
    {255, 0, 200},    // magenta
    {80, 80, 255},    // red
    {255, 255, 100},  // cyan
};
constexpr std::size_t kTrackColorCount =
    sizeof(kTrackColors) / sizeof(kTrackColors[0]);

constexpr int kLandmarkRadius = 3;
constexpr int kBoneThickness = 2;

// Converts one normalized coordinate to a pixel index along an axis of
// `extent` pixels.
//
// The clamp happens on the float, before rounding, on purpose: rounding first
// and clamping after would hand std::lround a value like 1e9 (or worse) and
// rely on it not overflowing. Clamping first means the integer conversion
// only ever sees a value inside the frame.
int ToPixel(float normalized, int extent) {
  const float scaled = normalized * static_cast<float>(extent);
  const float clamped = std::min(std::max(scaled, 0.0f), static_cast<float>(extent - 1));
  return static_cast<int>(std::lround(clamped));
}

// A landmark as a pixel point, or no value if its coordinates are not finite.
// NaN survives both comparisons in a clamp, so it has to be rejected here
// rather than relied on to clamp into range.
bool ToPixelPoint(const Point2& p, int width, int height, cv::Point& out) {
  if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
  out = cv::Point(ToPixel(p.x, width), ToPixel(p.y, height));
  return true;
}

}  // namespace

void DrawObservations(cv::Mat& bgr, const std::vector<HandObservation>& obs) {
  if (bgr.empty() || bgr.type() != CV_8UC3) return;

  const int width = bgr.cols;
  const int height = bgr.rows;

  for (const HandObservation& hand : obs) {
    const Bgr& v = kTrackColors[hand.track_id % kTrackColorCount];
    const cv::Scalar color(v.b, v.g, v.r);

    // Resolved once per hand, then reused by both the bones and the joints.
    std::array<cv::Point, 21> points{};
    std::array<bool, 21> valid{};
    for (std::size_t i = 0; i < hand.landmarks.size(); ++i) {
      valid[i] = ToPixelPoint(hand.landmarks[i], width, height, points[i]);
    }

    // Bones first, so the joints sit on top of them.
    for (const auto& c : kConnections) {
      if (!valid[c.first] || !valid[c.second]) continue;
      cv::line(bgr, points[c.first], points[c.second], color, kBoneThickness,
               cv::LINE_8);
    }

    for (std::size_t i = 0; i < points.size(); ++i) {
      if (!valid[i]) continue;
      cv::circle(bgr, points[i], kLandmarkRadius, color, cv::FILLED, cv::LINE_8);
    }
  }
}

Overlay::Overlay(std::string window_title) : title_(std::move(window_title)) {}

Overlay::~Overlay() {
  if (created_) cv::destroyWindow(title_);
}

void Overlay::Render(const ImageView& frame, const std::vector<HandObservation>& obs,
                     const std::string& hud) {
  if (frame.data == nullptr || frame.width <= 0 || frame.height <= 0) return;

  // The window is created on the first Render, not in the constructor, so an
  // Overlay can be constructed on a machine with no display without throwing.
  if (!created_) {
    cv::namedWindow(title_, cv::WINDOW_AUTOSIZE);
    created_ = true;
  }

  // A view over the source's own bytes -- no copy, and `stride` is honoured
  // because it may exceed width * 3.
  const cv::Mat view(frame.height, frame.width, CV_8UC3,
                     const_cast<std::uint8_t*>(frame.data),
                     static_cast<std::size_t>(frame.stride));

  // Cloned before drawing: the source pixels belong to the frame source,
  // which is free to reuse the buffer, and scribbling on them would corrupt
  // whatever else reads the same frame.
  cv::Mat canvas;
  if (frame.format == PixelFormat::Rgb8) {
    cv::cvtColor(view, canvas, cv::COLOR_RGB2BGR);
  } else {
    canvas = view.clone();
  }

  DrawObservations(canvas, obs);

  // The HUD, one line per '\n', in the top-left corner.
  int baseline_y = 24;
  std::size_t start = 0;
  while (start <= hud.size()) {
    const std::size_t end = hud.find('\n', start);
    const std::string line = hud.substr(start, end - start);
    if (!line.empty()) {
      cv::putText(canvas, line, cv::Point(10, baseline_y), cv::FONT_HERSHEY_SIMPLEX,
                  0.6, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
    baseline_y += 22;
    if (end == std::string::npos) break;
    start = end + 1;
  }

  cv::imshow(title_, canvas);

  // waitKey both pumps the event loop (without it nothing is drawn on any
  // backend) and is the only way to read a key, so the quit check is latched
  // here rather than in ShouldQuit.
  const int key = cv::waitKey(1) & 0xFF;
  if (key == 27 || key == 'q') quit_ = true;
}

bool WindowClosed(double visible_prop, bool was_visible) {
  // Negative means the backend does not implement WND_PROP_VISIBLE. The GTK
  // build used here returns -1 unconditionally, so a `< 1.0` test reports
  // "closed" on the very first frame and the loop exits after one iteration.
  // Unsupported is not closed: say no, and let the key be the quit route.
  if (visible_prop < 0.0) return false;

  // Supported, but the window has not been mapped yet. Creating a window and
  // asking the same instant whether it is visible is a race, not a close.
  if (!was_visible) return false;

  return visible_prop < 1.0;
}

bool Overlay::ShouldQuit() {
  if (quit_) return true;
  if (!created_) return false;

  // The title-bar close button delivers no key, so the window's own
  // visibility is the second half of the condition -- where the backend
  // supports reporting it.
  const double visible = cv::getWindowProperty(title_, cv::WND_PROP_VISIBLE);
  if (visible >= 1.0) was_visible_ = true;
  return WindowClosed(visible, was_visible_);
}

}  // namespace hc
