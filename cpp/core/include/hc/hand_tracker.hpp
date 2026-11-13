#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hc/hand_observation.hpp"
#include "hc/image_view.hpp"

namespace hc {

// Backend-agnostic hand tracker interface. Nothing here mentions ONNX
// Runtime, OpenCV, or any pixel-level type beyond ImageView, so the app layer
// and the tests both depend only on this.
class IHandTracker {
 public:
  virtual ~IHandTracker() = default;

  // Runs one frame through the pipeline and reports the hands found.
  //
  // `out` is CLEARED and refilled on every call, so a caller can hold one
  // vector across the whole session and never allocate per frame once it has
  // grown to its steady-state capacity. An empty `out` means "no hand this
  // frame", which is a normal result, not an error.
  //
  // `timestamp_us` is carried through to each observation's `timestamp_us`
  // unmodified; the tracker never reads the clock itself, so callers (and
  // tests) stay in control of time.
  //
  // All coordinates in `out` are normalized 0..1 against the full frame.
  virtual void Process(const ImageView& frame, std::int64_t timestamp_us,
                       std::vector<HandObservation>& out) = 0;

  // Drops all tracking state, so the next Process() starts from detection
  // again. Does not touch the Metrics instance the tracker was built with.
  virtual void Reset() = 0;
};

// Construction parameters for a concrete tracker.
struct HandTrackerConfig {
  // Absolute paths to the two ONNX model files. CMake fetches them and
  // exposes the paths as HC_PALM_MODEL / HC_LANDMARK_MODEL.
  std::string palm_model_path;
  std::string landmark_model_path;

  // Upper bound on simultaneously tracked hands.
  //
  // This default of 2 is FORWARD-LOOKING. As of this phase only the
  // single-hand path exists, and HandTrackerOnnx's constructor THROWS
  // std::invalid_argument on max_hands > 1 rather than silently tracking one
  // hand anyway. A caller today must therefore set this to 1 explicitly; the
  // default becomes usable when Task 10 adds two-hand association and lifts
  // the guard.
  int max_hands = 2;

  // A track is dropped when the landmark model's presence output falls below
  // this. 0.5 is MediaPipe's own ThresholdingCalculator value (models.md).
  float min_presence = 0.5f;

  // ONNX Runtime intra-op thread count. Pinned to 1 by default rather than
  // left at the ORT default so that timing measurements are reproducible and
  // the measurement task can vary it deliberately.
  int intra_op_threads = 1;
};

}  // namespace hc
