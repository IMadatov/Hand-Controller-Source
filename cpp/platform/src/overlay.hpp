#pragma once

#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "hc/hand_observation.hpp"
#include "hc/image_view.hpp"

namespace hc {

// Draws the landmarks and skeleton of every observation onto `bgr`, in place,
// colour-keyed by track_id so two hands are told apart on sight.
//
// THIS IS THE ONLY PLACE IN THE PROJECT THAT MULTIPLIES NORMALIZED
// COORDINATES BY PIXEL DIMENSIONS. Everything upstream speaks 0.0..1.0 in
// both axes, which is what keeps the pipeline resolution-independent; the
// conversion lives here, at the last possible moment, and nowhere else.
//
// `bgr` must be CV_8UC3. Landmarks outside 0.0..1.0 are clipped to the
// frame's edge rather than dropped, so a hand leaving the view is still
// visible as having gone off that particular side. Non-finite coordinates
// are skipped.
//
// Split out of Overlay so the drawing can be tested without a display, which
// is the whole reason it is a free function rather than a private method.
void DrawObservations(cv::Mat& bgr, const std::vector<HandObservation>& obs);

// A caption to draw beside one hand -- its current gesture, typically.
//
// Text, not a GestureKind: this layer TRANSLATES, it does not name. What a
// pose is called is hc_control's vocabulary (DescribeGesture), and a platform
// layer that spelled the gestures itself would hold one spelling per
// operating system.
struct HandLabel {
  std::uint32_t track_id;
  std::string text;
};

// Draws each label beside the hand with the matching `track_id`, in that
// hand's own track colour, so the caption and the skeleton it describes are
// the same colour.
//
// A label whose track_id matches no observation is SKIPPED rather than drawn
// somewhere arbitrary: the usual cause is a hand that left the frame on the
// very frame its gesture ended, and a caption floating over an empty corner
// would be a worse answer than none.
//
// Call after DrawObservations, so the text sits on top of the skeleton rather
// than under it.
void DrawHandLabels(cv::Mat& bgr, const std::vector<HandObservation>& obs,
                    const std::vector<HandLabel>& labels);

// Decides whether the preview window has been closed by the user.
//
// `visible_prop` is what getWindowProperty(WND_PROP_VISIBLE) returned,
// `image_width` is the width from getWindowImageRect (negative when the
// window is gone), and `was_visible` is whether a live window has ever been
// observed.
//
// Split out as a free function because it is the one piece of Overlay's quit
// logic testable without a display -- and it has now been wrong twice, in
// opposite directions, which is why it is worth this much comment.
//
// FIRST VERSION read only WND_PROP_VISIBLE and quit when it was below 1. On
// the trimmed GTK build here that property returns -1 unconditionally, so the
// loop exited after a single frame.
//
// SECOND VERSION treated a negative value as "unsupported, so not closed",
// which fixed that and made closing the window do nothing at all: the next
// imshow silently recreated it. Measured on 2026-11-15 with a real run -- the
// user closed the window twice and the log shows it recreated both times.
//
// THE DISCRIMINATOR IS THE IMAGE RECT, measured rather than guessed:
//
//   state                VISIBLE  AUTOSIZE  getWindowImageRect
//   no window ever          -1       -1     throws
//   window up               -1        1     160x120
//   after it is destroyed   -1        1     -1 x -1
//
// So AUTOSIZE goes stale and VISIBLE is useless, and only the rect
// distinguishes a live window from a destroyed one on this backend.
bool WindowClosed(double visible_prop, int image_width, bool was_visible);

// The debug preview window. Along with the frame sources, this is the only
// part of the project allowed to touch OpenCV's UI; the pipeline must run
// fully headless with this class absent, so nothing else may depend on it.
class Overlay {
 public:
  explicit Overlay(std::string window_title);
  ~Overlay();

  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // Copies `frame`, draws `obs`, `labels` and `hud` onto the copy, and shows
  // it. The source pixels are never modified: ImageView is a read-only view
  // into storage the frame source owns and may reuse.
  //
  // Also pumps the UI event loop, which is why ShouldQuit's answer is only
  // current after a Render call.
  void Render(const ImageView& frame, const std::vector<HandObservation>& obs,
              const std::vector<HandLabel>& labels, const std::string& hud);

  // True once the user has asked to quit: Escape or 'q' pressed, or the
  // window closed. Both conditions matter -- a window closed by its title-bar
  // button never delivers a key, and waitKey never reports the close.
  //
  // Where the backend does not report visibility (see WindowClosed), the key
  // is the only quit route and closing the window will not stop the loop.
  bool ShouldQuit();

 private:
  std::string title_;
  bool created_ = false;
  bool quit_ = false;
  // Latched once the window is observed visible, so the gap between creating
  // the window and the compositor mapping it is not mistaken for a close.
  bool was_visible_ = false;
};

}  // namespace hc
