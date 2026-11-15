#pragma once

#include <array>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"

namespace hc {

// The derived quantities a gesture might be built from, computed once per
// hand per frame so no recognizer rule recomputes them.
//
// Everything here is a property of the POSE, not of the hand's identity:
// handedness and track_id are deliberately not read. Phase 1 measured
// handedness confidence as low as 0.575 on a two-hand frame, so a feature
// that varied with the label would be unreliable by construction.
struct HandFeatures {
  // Per finger, thumb first: index 0 is the thumb, then index, middle, ring,
  // pinky. True when the finger is extended rather than curled.
  std::array<bool, 5> extended;

  // Thumb-tip to index-tip distance, divided by palm_size so it does not
  // change as the hand moves toward or away from the camera. A raw distance
  // would make every pinch threshold depth-dependent, which is the
  // resolution-locking this rebuild exists to remove.
  float pinch_distance;

  // Centroid of the wrist and the four finger knuckles. Normalized to the
  // frame, like every coordinate in the core.
  Point2 palm_center;

  // Wrist to middle-finger knuckle, the hand's own scale reference.
  // GUARANTEED POSITIVE: it is a divisor, and a degenerate hand (every
  // landmark identical, which is what a collapsed detection looks like)
  // would otherwise make it zero.
  float palm_size;
};

// Computes the features of one hand. Pure: no clock, no I/O, no allocation.
//
// Landmarks outside 0..1 are accepted as-is; phase 1's tracker clamps
// nothing, and a hand partly out of frame legitimately produces them.
HandFeatures ComputeFeatures(const HandObservation& obs);

}  // namespace hc
