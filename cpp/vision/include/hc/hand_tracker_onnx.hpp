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
// Tracks up to `HandTrackerConfig::max_hands` hands, one independent slot
// each. Each slot carries its own track id, its own ROI, and its own
// lifetime; slots never share state, so one hand leaving the frame does not
// disturb the others.
//
// Per frame, in this order:
//
//  1. If ANY slot is inactive, the whole frame is letterboxed into the palm
//     detector's 192x192 input (a full-frame RotatedRect squared on its long
//     side, so the short axis expands past the frame edge and the warp's
//     BORDER_ZERO fill supplies the padding) and the detector runs ONCE.
//     Detections are mapped back to frame coordinates, matched against the
//     live slots with hc::AssignDetectionsToSlots, and the ones that match a
//     live slot are discarded as hands already being tracked. The remainder
//     fill free slots, in order, until none is left -- that exhaustion IS the
//     max_hands cap.
//  2. Every active slot (including any seeded in step 1) is warped from its
//     own ROI into the landmark model's 224x224 input and run. A slot whose
//     presence falls below HandTrackerConfig::min_presence is deactivated,
//     and only that slot.
//
// Two ordering consequences worth knowing before changing anything here:
//
//  - The step-1 decision reads the slot state at frame ENTRY. A slot that
//    dies in step 2 therefore re-triggers detection on the NEXT frame, not
//    the current one.
//  - Because step 1 fires whenever ANY slot is empty, a scene with fewer
//    hands than max_hands runs the palm detector on EVERY frame. That is
//    MediaPipe's behaviour and is intentional in this phase (there is no
//    re-detection throttle), but it means per-frame cost depends on how many
//    hands are visible, not just on max_hands.
//
// While every slot is full the detector is skipped entirely and each slot
// rides on the ROI derived from its own previous-frame landmarks
// (LandmarkRoiOptions).
//
// The palm detector's 192x192 input size is PINNED, not adapted: the decode
// scales and the SSD anchor grid are tied to that specific architecture. The
// constructor throws if the loaded model's input side is anything else.
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
  // Throws: Ort::Exception if either model is missing or malformed;
  // std::runtime_error if a model's tensor layout or input size differs from
  // what cpp/docs/models.md records; std::invalid_argument if
  // config.max_hands is < 1, or large enough that 2*max_hands - 1 exceeds
  // hc::kMaxAssociationEntries (i.e. > 32), past which the association layer
  // would start ignoring slots instead of matching them.
  // `metrics` must outlive this object.
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
