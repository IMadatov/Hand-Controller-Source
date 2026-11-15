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

  // Upper bound on simultaneously tracked hands. HandTrackerOnnx honours it
  // for real: it holds exactly this many tracking slots, and a detection that
  // finds no free slot is dropped.
  //
  // Note this is a CEILING, not a target. The tracker keeps re-running the
  // palm detector every frame for as long as any slot is empty, so setting
  // this to 2 in a scene that only ever shows one hand costs a palm detection
  // on every single frame. Set it to the number of hands you actually intend
  // to track.
  //
  // Must be >= 1, and small enough that 2*max_hands - 1 stays within
  // hc::kMaxAssociationEntries (so, <= 32); the constructor throws otherwise.
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
