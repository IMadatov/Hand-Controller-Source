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
// Values are B, G, R -- the order OpenCV draws in, not RGB. Naming these
// from the RGB reading is how the first two entries came to be commented as
// their own opposites.
constexpr Bgr kTrackColors[] = {
    {255, 128, 0},    // azure
    {0, 220, 255},    // orange
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

// getWindowImageRect throws for a name no window was ever created under, and
// returns a negative rect for one that has been destroyed. Both mean "not
// there" to the caller, so the exception is folded into the same answer
// rather than propagated into a frame loop.
int WindowImageWidth(const std::string& title) {
  try {
    return cv::getWindowImageRect(title).width;
  } catch (const cv::Exception&) {
    return -1;
  }
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

void DrawHandLabels(cv::Mat& bgr, const std::vector<HandObservation>& obs,
                    const std::vector<HandLabel>& labels) {
  if (bgr.empty() || bgr.type() != CV_8UC3) return;

  const int width = bgr.cols;
  const int height = bgr.rows;

  for (const HandLabel& label : labels) {
    if (label.text.empty()) continue;

    for (const HandObservation& hand : obs) {
      if (hand.track_id != label.track_id) continue;

      // Anchored to the hand's HIGHEST valid landmark, so the caption sits
      // above the fingertips of an upright hand instead of across them. The
      // topmost point is used rather than the wrist because the wrist is at
      // the bottom of an upright hand and a caption there lands on the arm.
      int top_x = 0;
      int top_y = height;
      bool found = false;
      for (const Point2& p : hand.landmarks) {
        cv::Point pixel;
        if (!ToPixelPoint(p, width, height, pixel)) continue;
        if (!found || pixel.y < top_y) {
          top_x = pixel.x;
          top_y = pixel.y;
          found = true;
        }
      }
      if (!found) break;  // Every landmark non-finite; nothing to anchor to.

      // Clamped so a hand at the top or the right edge still shows its whole
      // caption. cv::putText silently clips text drawn off-canvas, which
      // would make the label vanish exactly when the hand is at an edge --
      // the moment a user is most likely to be wondering what it says.
      const int kTextHeight = 18;
      const int kGap = 8;
      const double kScale = 0.5;
      const int baseline =
          std::min(std::max(top_y - kGap, kTextHeight), height - 2);
      const cv::Size text =
          cv::getTextSize(label.text, cv::FONT_HERSHEY_SIMPLEX, kScale, 1,
                          nullptr);
      const int x = std::min(std::max(top_x - text.width / 2, 2),
                             std::max(2, width - text.width - 2));

      const Bgr& v = kTrackColors[hand.track_id % kTrackColorCount];

      // Drawn twice: a dark outline under the coloured text, because a thin
      // coloured glyph over a camera image is unreadable about half the time
      // and the palette is chosen for the skeleton, not for contrast.
      cv::putText(bgr, label.text, cv::Point(x, baseline),
                  cv::FONT_HERSHEY_SIMPLEX, kScale, cv::Scalar(0, 0, 0), 3,
                  cv::LINE_AA);
      cv::putText(bgr, label.text, cv::Point(x, baseline),
                  cv::FONT_HERSHEY_SIMPLEX, kScale,
                  cv::Scalar(v.b, v.g, v.r), 1, cv::LINE_AA);
      break;  // One hand per label.
    }
  }
}

Overlay::Overlay(std::string window_title) : title_(std::move(window_title)) {}

Overlay::~Overlay() {
  if (created_) cv::destroyWindow(title_);
}

void Overlay::Render(const ImageView& frame, const std::vector<HandObservation>& obs,
                     const std::vector<HandLabel>& labels,
                     const std::string& hud) {
  // Once either quit route has fired there is nothing left to draw, and
  // drawing anyway would recreate a window the user closed.
  if (quit_) return;

  if (frame.data == nullptr || frame.width <= 0 || frame.height <= 0) return;

  // The window is created on the first Render, not in the constructor, so an
  // Overlay can be constructed on a machine with no display without throwing.
  if (!created_) {
    cv::namedWindow(title_, cv::WINDOW_AUTOSIZE);
    created_ = true;
  } else {
    // CHECKED BEFORE imshow, and that ordering is the whole fix: imshow
    // silently RECREATES a window the user has closed, so a check afterwards
    // always sees a live window and the close is never noticed. Measured --
    // the real run's log shows the window recreated twice.
    const int width = WindowImageWidth(title_);
    if (width > 0) was_visible_ = true;
    if (WindowClosed(cv::getWindowProperty(title_, cv::WND_PROP_VISIBLE),
                     width, was_visible_)) {
      quit_ = true;
      return;
    }
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
  DrawHandLabels(canvas, obs, labels);

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

bool WindowClosed(double visible_prop, int image_width, bool was_visible) {
  // Nothing has been seen alive yet. Creating a window and asking the same
  // instant whether it is still there is a race, not a close.
  if (!was_visible) return false;

  // The window existed and now has no image rect: it is gone. This is the
  // only signal that works on the GTK build here -- see the header's table.
  if (image_width < 0) return true;

  // A live rect, and a backend that does not report visibility. Not closed.
  if (visible_prop < 0.0) return false;

  return visible_prop < 1.0;
}


bool Overlay::ShouldQuit() {
  // Both quit routes are latched in Render: the key by waitKey, and the
  // closed window by the rect check that has to happen before imshow. Asking
  // the window anything here would be asking after it was recreated.
  return quit_;
}

}  // namespace hc
