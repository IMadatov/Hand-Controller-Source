#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/image_view.hpp"
#include "hc/metrics.hpp"

namespace hc {

// ONNX Runtime implementation of IHandTracker: MediaPipe's two-model
// palm-detect / hand-landmark pipeline, wired as a detect-then-track state
// machine.
//
// Per frame, when no hand is currently tracked: the whole frame is
// letterboxed into the palm detector's 192x192 input (a full-frame
// RotatedRect squared on its long side, so the short axis expands past the
// frame edge and the warp's BORDER_ZERO fill supplies the padding), the
// detector runs, the best detection is decoded and mapped back to frame
// coordinates, and a ROI is derived with PalmRoiOptions. When a hand IS
// tracked, the detector is skipped entirely and the ROI derived from the
// previous frame's landmarks (LandmarkRoiOptions) is used instead. Either
// way the ROI is warped into the landmark model's 224x224 input, the model
// runs, and the track is dropped when presence falls below
// HandTrackerConfig::min_presence.
//
// Task 8 scope: ONE hand. `max_hands > 1` does not yet yield more than one
// observation; multi-hand association is Tasks 9/10.
//
// ONNX Runtime types are deliberately kept out of this header (pimpl), so
// that including it does not drag <onnxruntime_cxx_api.h> into the app or
// the tests.
//
// Not copyable, not movable, and not thread-safe: it owns ORT sessions and
// holds a reference to a Metrics instance that must outlive it.
class HandTrackerOnnx : public IHandTracker {
 public:
  // Loads both models eagerly and generates the SSD anchor grid once.
  // Throws (Ort::Exception, or std::runtime_error for a shape/name mismatch)
  // if either model is missing or does not have the tensor layout recorded
  // in cpp/docs/models.md. `metrics` must outlive this object.
  HandTrackerOnnx(const HandTrackerConfig& config, Metrics& metrics);
  ~HandTrackerOnnx() override;

  HandTrackerOnnx(const HandTrackerOnnx&) = delete;
  HandTrackerOnnx& operator=(const HandTrackerOnnx&) = delete;

  void Process(const ImageView& frame, std::int64_t timestamp_us,
               std::vector<HandObservation>& out) override;
  void Reset() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hc
