#pragma once

#include <array>
#include <cstdint>

#include "hc/geometry.hpp"

namespace hc {

// Which hand a track is believed to be, as reported by the landmark model.
enum class Handedness { Unknown, Left, Right };

// A single hand's tracked state at one point in time. All landmark
// coordinates are normalized to the full frame, in the same sense as Point2.
struct HandObservation {
  std::uint32_t track_id;
  Handedness handedness;
  float handedness_score;
  float presence;
  std::array<Point2, 21> landmarks;
  std::int64_t timestamp_us;
};

}  // namespace hc
