#pragma once

// Synthetic hand poses for tests that need a hand without a camera.
//
// These are the layout test_gesture_recognizer.cpp established and
// cpp/tools/dump_contract.cpp reuses: an upright right hand, wrist at the
// bottom, finger chains running upward so each tip sits well beyond its PIP,
// thumb splayed out to the side.
//
// Built with WIDE margins against the feature thresholds in
// hand_features.cpp -- an extended finger is far past its PIP, a splayed
// thumb is far past 0.9 palm-sizes from the index knuckle. A test using these
// should fail because the behaviour under test is wrong, not because a pose
// sat on a feature boundary. Feature edges are test_hand_features.cpp's job.

#include <cstdint>

#include "hc/hand_observation.hpp"

namespace hc_test {

// MediaPipe's 21-landmark indices, named so the poses below read as anatomy
// rather than as magic numbers.
enum Lm {
  kWrist = 0,
  kThumbCmc = 1, kThumbMcp = 2, kThumbIp = 3, kThumbTip = 4,
  kIndexMcp = 5, kIndexPip = 6, kIndexDip = 7, kIndexTip = 8,
  kMiddleMcp = 9, kMiddlePip = 10, kMiddleDip = 11, kMiddleTip = 12,
  kRingMcp = 13, kRingPip = 14, kRingDip = 15, kRingTip = 16,
  kPinkyMcp = 17, kPinkyPip = 18, kPinkyDip = 19, kPinkyTip = 20,
};

// Column offsets of the four finger chains, index first.
inline constexpr float kCol[4] = {-0.05f, -0.01f, 0.03f, 0.07f};
inline constexpr int kMcp[4] = {kIndexMcp, kMiddleMcp, kRingMcp, kPinkyMcp};
inline constexpr int kPip[4] = {kIndexPip, kMiddlePip, kRingPip, kPinkyPip};
inline constexpr int kDip[4] = {kIndexDip, kMiddleDip, kRingDip, kPinkyDip};
inline constexpr int kTip[4] = {kIndexTip, kMiddleTip, kRingTip, kPinkyTip};

inline hc::HandObservation OpenPalm(std::uint32_t track_id,
                                    hc::Handedness hand, hc::Point2 origin) {
  hc::HandObservation o{};
  o.track_id = track_id;
  o.handedness = hand;
  o.handedness_score = 0.9f;
  o.presence = 1.0f;
  o.timestamp_us = 0;

  const auto put = [&](int i, float dx, float dy) {
    o.landmarks[i] = hc::Point2{origin.x + dx, origin.y + dy};
  };
  put(kWrist, 0.0f, 0.0f);
  put(kThumbCmc, -0.03f, -0.02f);
  put(kThumbMcp, -0.07f, -0.05f);
  put(kThumbIp, -0.11f, -0.07f);
  put(kThumbTip, -0.20f, -0.09f);
  for (int f = 0; f < 4; ++f) {
    put(kMcp[f], kCol[f], -0.10f);
    put(kPip[f], kCol[f], -0.18f);
    put(kDip[f], kCol[f], -0.24f);
    put(kTip[f], kCol[f], -0.30f);
  }
  return o;
}

// Curls finger `f` (0 = index) so its tip falls nearer the wrist than its
// PIP, which is what hand_features.cpp reads as "not extended".
inline void Curl(hc::HandObservation& o, int f) {
  const hc::Point2 pip = o.landmarks[kPip[f]];
  o.landmarks[kDip[f]] = hc::Point2{pip.x, pip.y + 0.03f};
  o.landmarks[kTip[f]] = hc::Point2{pip.x, pip.y + 0.06f};
}

// Folds the thumb across the palm, landing its tip beside the index knuckle
// at roughly 0.14 palm-sizes of splay.
inline void FoldThumb(hc::HandObservation& o) {
  const hc::Point2 index_mcp = o.landmarks[kIndexMcp];
  o.landmarks[kThumbIp] =
      hc::Point2{index_mcp.x - 0.01f, index_mcp.y + 0.02f};
  o.landmarks[kThumbTip] =
      hc::Point2{index_mcp.x + 0.01f, index_mcp.y + 0.01f};
}

// Index only: the pose the shipped configuration binds to cursor movement.
inline hc::HandObservation PointIndex(std::uint32_t track_id,
                                      hc::Handedness hand,
                                      hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(track_id, hand, origin);
  for (int f = 1; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Index and middle: the shipped configuration's click.
inline hc::HandObservation TwoFingers(std::uint32_t track_id,
                                      hc::Handedness hand,
                                      hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(track_id, hand, origin);
  for (int f = 2; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Thumb only: all four fingers curled, thumb left splayed.
inline hc::HandObservation ThumbUp(std::uint32_t track_id,
                                   hc::Handedness hand, hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(track_id, hand, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  return o;
}

inline hc::HandObservation Fist(std::uint32_t track_id, hc::Handedness hand,
                                hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(track_id, hand, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

}  // namespace hc_test
