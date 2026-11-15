#include "hc/hand_features.hpp"

#include <algorithm>
#include <cmath>

namespace hc {
namespace {

// MediaPipe's 21-landmark layout.
constexpr int kWrist = 0;
constexpr int kThumbMcp = 2;
constexpr int kThumbTip = 4;
constexpr int kIndexMcp = 5;
constexpr int kIndexTip = 8;
constexpr int kMiddleMcp = 9;
constexpr int kRingMcp = 13;
constexpr int kPinkyMcp = 17;

// Per finger: the PIP joint and the tip. The thumb is absent on purpose --
// it does not obey the same rule and is handled separately below.
constexpr int kFingerPip[4] = {6, 10, 14, 18};
constexpr int kFingerTip[4] = {8, 12, 16, 20};

// Knuckles, used for the palm centroid and the scale reference.
constexpr int kKnuckles[4] = {kIndexMcp, kMiddleMcp, kRingMcp, kPinkyMcp};

// Smallest palm_size to report. The value itself is arbitrary; what matters
// is that it is strictly positive, because palm_size divides the
// scale-invariant quantities and a degenerate hand would otherwise make it
// zero. See HandFeatures::palm_size.
constexpr float kMinPalmSize = 1e-4f;

float Distance(const Point2& a, const Point2& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

HandFeatures ComputeFeatures(const HandObservation& obs) {
  const auto& lm = obs.landmarks;
  HandFeatures f{};

  // Scale first: everything below that must be scale-invariant divides by it.
  f.palm_size = std::max(Distance(lm[kWrist], lm[kMiddleMcp]), kMinPalmSize);

  // Centroid of the wrist and the four knuckles.
  float cx = lm[kWrist].x;
  float cy = lm[kWrist].y;
  for (const int k : kKnuckles) {
    cx += lm[k].x;
    cy += lm[k].y;
  }
  f.palm_center = Point2{cx / 5.0f, cy / 5.0f};

  // The four fingers: extended when the tip is further from the wrist than
  // the PIP is. Phrased as a distance rather than as "tip is above PIP in y"
  // so it survives a rotated hand -- a sideways or upside-down hand is an
  // ordinary thing for a user to do, and the y-comparison the Python version
  // used silently inverts for it.
  for (int i = 0; i < 4; ++i) {
    f.extended[i + 1] = Distance(lm[kFingerTip[i]], lm[kWrist]) >
                        Distance(lm[kFingerPip[i]], lm[kWrist]);
  }

  // The thumb folds ACROSS the palm instead of curling into it, so its tip
  // stays about as far from the wrist either way and the finger rule above
  // reports it wrong. Measure its splay from the palm instead: an extended
  // thumb sits well away from the index knuckle, a folded one lies over the
  // palm and close to it. Normalized by palm_size, so it is depth-invariant
  // like the pinch.
  //
  // NOT YET CALIBRATED AGAINST A REAL HAND. 0.9 separates the synthetic
  // poses in test_hand_features.cpp with margin, but a live probe produced
  // only three usable samples, which is not enough to set a threshold. Tune
  // it in Task 3 against recorded landmark sequences, where the gesture
  // thresholds are being set anyway, and move it into GestureConfig if the
  // right value turns out to depend on the user.
  const float thumb_splay = Distance(lm[kThumbTip], lm[kIndexMcp]) / f.palm_size;
  f.extended[0] = thumb_splay > 0.9f;

  f.pinch_distance = Distance(lm[kThumbTip], lm[kIndexTip]) / f.palm_size;

  // Silences an unused-constant warning while documenting that the thumb MCP
  // is deliberately not part of the rule above.
  (void)kThumbMcp;
  return f;
}

}  // namespace hc
