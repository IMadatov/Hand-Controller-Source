#include "hc/gesture_recognizer.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hc/gesture_event.hpp"
#include "hc/hand_features.hpp"
#include "hc/hand_observation.hpp"

using hc::GestureConfig;
using hc::GestureEvent;
using hc::GestureKind;
using hc::GestureRecognizer;
using hc::GestureScore;
using hc::Handedness;
using hc::HandObservation;
using hc::Point2;
using Phase = hc::GestureEvent::Phase;

namespace {

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
constexpr float kCol[4] = {-0.05f, -0.01f, 0.03f, 0.07f};
constexpr int kMcp[4] = {kIndexMcp, kMiddleMcp, kRingMcp, kPinkyMcp};
constexpr int kPip[4] = {kIndexPip, kMiddlePip, kRingPip, kPinkyPip};
constexpr int kDip[4] = {kIndexDip, kMiddleDip, kRingDip, kPinkyDip};
constexpr int kTip[4] = {kIndexTip, kMiddleTip, kRingTip, kPinkyTip};

// Wrist to middle-finger knuckle for the layout below, i.e. what
// ComputeFeatures will report as palm_size. Written out because the pinch
// poses need it to turn a requested palm-size ratio into a coordinate.
constexpr float kPalmSize = 0.100499f;  // hypot(0.01, 0.10)

// These poses are deliberately built with WIDE margins against the feature
// thresholds in hand_features.cpp -- an extended finger is far past its PIP,
// a splayed thumb is far past 0.9 palm-sizes from the index knuckle. The
// tests in this file are about latching, phases and per-track independence;
// if one of them fails it should be because the state machine is wrong, not
// because a pose sat on a feature boundary. Feature edges are
// test_hand_features.cpp's job.
//
// An upright hand: wrist at the bottom, finger chains running upward so each
// tip sits well beyond its PIP, thumb splayed out to the side.
HandObservation OpenPalm(std::uint32_t track_id = 1,
                         Point2 origin = {0.5f, 0.8f}) {
  HandObservation o{};
  o.track_id = track_id;
  o.handedness = Handedness::Right;
  o.handedness_score = 0.9f;
  o.presence = 1.0f;
  o.timestamp_us = 0;

  const auto put = [&](int i, float dx, float dy) {
    o.landmarks[i] = Point2{origin.x + dx, origin.y + dy};
  };
  put(kWrist, 0.0f, 0.0f);

  // Thumb, out to the side and clear of the palm. Splay works out to about
  // 1.5 palm-sizes against a 0.9 threshold.
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
void Curl(HandObservation& o, int f) {
  const Point2 pip = o.landmarks[kPip[f]];
  o.landmarks[kDip[f]] = Point2{pip.x, pip.y + 0.03f};
  o.landmarks[kTip[f]] = Point2{pip.x, pip.y + 0.06f};
}

// Folds the thumb across the palm, landing its tip beside the index knuckle
// at roughly 0.14 palm-sizes of splay.
void FoldThumb(HandObservation& o) {
  const Point2 index_mcp = o.landmarks[kIndexMcp];
  o.landmarks[kThumbIp] = Point2{index_mcp.x - 0.01f, index_mcp.y + 0.02f};
  o.landmarks[kThumbTip] = Point2{index_mcp.x + 0.01f, index_mcp.y + 0.01f};
}

HandObservation Fist(std::uint32_t track_id = 1,
                     Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// A pinch, with the thumb tip placed `ratio` palm-sizes from the index tip so
// the test can drive pinch_distance directly.
//
// Middle, ring and pinky are curled, which is both what a real pinch looks
// like and what keeps this pose from also scoring as an open palm -- with all
// five extended, Pinch and OpenPalm would both score 1.0 and the test would
// be asserting a tie-break rather than a gesture.
HandObservation PinchAt(float ratio, std::uint32_t track_id = 1,
                        Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 1; f < 4; ++f) Curl(o, f);
  const Point2 index_tip = o.landmarks[kIndexTip];
  o.landmarks[kThumbTip] =
      Point2{index_tip.x + ratio * kPalmSize, index_tip.y};
  return o;
}

// Only the thumb extended: all four fingers curled, thumb left splayed.
HandObservation ThumbUp(std::uint32_t track_id = 1,
                        Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  return o;
}

// Index only: middle, ring and pinky curled, thumb folded in.
HandObservation PointIndex(std::uint32_t track_id = 1,
                           Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 1; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Index and middle: ring and pinky curled, thumb folded in.
HandObservation TwoFingers(std::uint32_t track_id = 1,
                           Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 2; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Pinky only: index, middle and ring curled, thumb folded in. main.py bound
// this to the left arrow key.
HandObservation PinkyOnly(std::uint32_t track_id = 1,
                          Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  for (int f = 0; f < 3; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// All four fingers, thumb folded in. main.py bound this to the down arrow,
// and it differs from an open palm by the thumb alone.
HandObservation FourFingers(std::uint32_t track_id = 1,
                            Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = OpenPalm(track_id, origin);
  FoldThumb(o);
  return o;
}

// --- assertions over an event stream -------------------------------------

int CountPhase(const std::vector<GestureEvent>& ev, Phase phase) {
  int n = 0;
  for (const auto& e : ev) {
    if (e.phase == phase) ++n;
  }
  return n;
}

int CountFor(const std::vector<GestureEvent>& ev, std::uint32_t track_id,
             Phase phase) {
  int n = 0;
  for (const auto& e : ev) {
    if (e.track_id == track_id && e.phase == phase) ++n;
  }
  return n;
}

// Runs `frames` frames of a single unchanging pose and accumulates every
// event, so a test can assert over the whole stream rather than frame by
// frame.
std::vector<GestureEvent> RunPose(GestureRecognizer& r,
                                  const HandObservation& pose, int frames) {
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;
  for (int i = 0; i < frames; ++i) {
    r.Process({pose}, i * 1000, out);
    all.insert(all.end(), out.begin(), out.end());
  }
  return all;
}

}  // namespace

// --- the score function, separately from the latching around it ----------

TEST(GestureScore, RecognizesEachPoseAsItsOwnGesture) {
  const GestureConfig c = GestureConfig::Default();
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::OpenPalm,
                               hc::ComputeFeatures(OpenPalm()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::Fist,
                               hc::ComputeFeatures(Fist()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::ThumbUp,
                               hc::ComputeFeatures(ThumbUp()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::PointIndex,
                               hc::ComputeFeatures(PointIndex()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::TwoFingers,
                               hc::ComputeFeatures(TwoFingers()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::PinkyOnly,
                               hc::ComputeFeatures(PinkyOnly()), c), 1.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::FourFingers,
                               hc::ComputeFeatures(FourFingers()), c), 1.0f);
}

// Every pose must latch as its OWN kind and no other. With nine kinds scored
// from the same five booleans, two of them differing by a single finger, this
// is the test that catches a target pattern typed wrong -- a mistake that
// would otherwise show up as the wrong command firing on a real hand.
TEST(GestureRecognizer, EachPoseLatchesAsItsOwnKind) {
  struct Case {
    const char* label;
    HandObservation pose;
    GestureKind expected;
  };
  const Case cases[] = {
      {"open_palm", OpenPalm(), GestureKind::OpenPalm},
      {"fist", Fist(), GestureKind::Fist},
      {"pinch", PinchAt(0.05f), GestureKind::Pinch},
      {"point_index", PointIndex(), GestureKind::PointIndex},
      {"two_fingers", TwoFingers(), GestureKind::TwoFingers},
      {"thumb_up", ThumbUp(), GestureKind::ThumbUp},
      {"pinky_only", PinkyOnly(), GestureKind::PinkyOnly},
      {"four_fingers", FourFingers(), GestureKind::FourFingers},
  };

  for (const Case& c : cases) {
    GestureRecognizer r(GestureConfig::Default());
    std::vector<GestureEvent> out;
    r.Process({c.pose}, 0, out);
    ASSERT_EQ(out.size(), 1u) << c.label << " produced no gesture";
    EXPECT_EQ(out[0].kind, c.expected)
        << c.label << " latched as " << hc::GestureKindName(out[0].kind);
  }
}

// FourFingers and OpenPalm differ by the thumb alone, so at 0.9 enter each
// must exclude the other: 4/5 = 0.8 does not clear the threshold. If the
// thumb were ignored they would be indistinguishable.
TEST(GestureRecognizer, OpenPalmAndFourFingersDoNotCollide) {
  const GestureConfig c = GestureConfig::Default();
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::FourFingers,
                               hc::ComputeFeatures(OpenPalm()), c), 0.8f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::OpenPalm,
                               hc::ComputeFeatures(FourFingers()), c), 0.8f);
  const float enter =
      c.thresholds[static_cast<std::size_t>(GestureKind::OpenPalm)].enter;
  EXPECT_LT(0.8f, enter) << "one finger apart must not be enough to latch";
}

TEST(GestureScore, NoneNeverScores) {
  const GestureConfig c = GestureConfig::Default();
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::None,
                               hc::ComputeFeatures(OpenPalm()), c), 0.0f);
  EXPECT_FLOAT_EQ(GestureScore(GestureKind::None,
                               hc::ComputeFeatures(Fist()), c), 0.0f);
}

TEST(GestureScore, PinchRisesAsTheFingersClose) {
  const GestureConfig c = GestureConfig::Default();
  const float wide = GestureScore(GestureKind::Pinch,
                                  hc::ComputeFeatures(PinchAt(0.9f)), c);
  const float close = GestureScore(GestureKind::Pinch,
                                   hc::ComputeFeatures(PinchAt(0.1f)), c);
  EXPECT_LT(wide, close);
  EXPECT_NEAR(GestureScore(GestureKind::Pinch,
                           hc::ComputeFeatures(PinchAt(0.0f)), c),
              1.0f, 1e-4f);
}

// An open palm must not read as a pinch just because a threshold is loose:
// the two are the most likely pair to collide, since a pinch keeps the index
// extended.
TEST(GestureScore, OpenPalmDoesNotScoreAsPinch) {
  const GestureConfig c = GestureConfig::Default();
  const float s = GestureScore(GestureKind::Pinch,
                               hc::ComputeFeatures(OpenPalm()), c);
  EXPECT_LT(s, c.thresholds[static_cast<std::size_t>(GestureKind::Pinch)].exit);
}

TEST(GestureConfigTest, EveryEnterThresholdIsStricterThanItsExit) {
  const GestureConfig c = GestureConfig::Default();
  for (std::size_t i = 1; i < hc::kGestureKindCount; ++i) {
    EXPECT_GT(c.thresholds[i].enter, c.thresholds[i].exit)
        << "kind index " << i
        << ": enter must exceed exit or there is no hysteresis";
  }
}

TEST(GestureConfigTest, DefaultConfirmsATakeoverOverMoreThanOneFrame) {
  const GestureConfig c = GestureConfig::Default();
  EXPECT_GE(c.steal_confirm_frames, 1)
      << "a value below 1 means an instant takeover, which is the chatter the "
         "hysteresis exists to prevent";
}

// --- takeover between gestures one finger apart --------------------------
//
// Five pairs in the vocabulary differ by a single finger, so the exit
// threshold that tolerates one finger of flicker (4/5 = 0.8, above the 0.7
// exit) also makes the transition between them impossible without a takeover.
// point_index -> two_fingers is the default configuration's primary
// interaction -- point to move the cursor, raise the middle finger to click --
// so it is the pair these tests use.

TEST(GestureRecognizer, ABetterScoringGestureTakesOverAfterConfirmation) {
  const GestureConfig c = GestureConfig::Default();
  GestureRecognizer r(c);
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  const auto feed = [&](const HandObservation& pose, int frames) {
    for (int i = 0; i < frames; ++i) {
      r.Process({pose}, 0, out);
      all.insert(all.end(), out.begin(), out.end());
    }
  };

  // Long enough after the confirmation count that the new gesture has room to
  // begin and be held, rather than landing on the last frame.
  feed(PointIndex(), 4);
  feed(TwoFingers(), c.steal_confirm_frames + 4);

  ASSERT_GE(all.size(), 3u);
  EXPECT_EQ(all.front().kind, GestureKind::PointIndex);
  EXPECT_EQ(all.front().phase, Phase::Began);

  // The old gesture ended exactly once and the new one began exactly once.
  int point_ended = 0;
  int two_began = 0;
  for (const auto& e : all) {
    if (e.kind == GestureKind::PointIndex && e.phase == Phase::Ended) {
      ++point_ended;
    }
    if (e.kind == GestureKind::TwoFingers && e.phase == Phase::Began) {
      ++two_began;
    }
  }
  EXPECT_EQ(point_ended, 1);
  EXPECT_EQ(two_began, 1);

  // And the stream finishes on the new gesture, still held.
  EXPECT_EQ(all.back().kind, GestureKind::TwoFingers);
  EXPECT_EQ(all.back().phase, Phase::Held);
}

// A takeover ends one gesture and begins another, and a one-shot binding
// fires on that begin. If a single bad frame of landmarks were enough, a
// finger wobbling between two bound poses would produce a click per wobble.
TEST(GestureRecognizer, ASingleWobbleFrameDoesNotTakeOver) {
  const GestureConfig c = GestureConfig::Default();
  ASSERT_GT(c.steal_confirm_frames, 1)
      << "this test is meaningless if one frame is enough";

  GestureRecognizer r(c);
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  // Alternating, so the challenger never wins twice in a row.
  for (int i = 0; i < 40; ++i) {
    r.Process({i % 2 == 0 ? PointIndex() : TwoFingers()}, i, out);
    all.insert(all.end(), out.begin(), out.end());
  }

  EXPECT_EQ(CountPhase(all, Phase::Began), 1);
  EXPECT_EQ(CountPhase(all, Phase::Ended), 0);
  for (const auto& e : all) EXPECT_EQ(e.kind, GestureKind::PointIndex);
}

// The release of one gesture and the press of an unrelated one must not arrive
// from the same frame: a binding that maps them to key_press and key_release
// would otherwise see them out of order within one dispatch.
TEST(GestureRecognizer, TakeoverEndsAndBeginsOnSeparateFrames) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;

  for (int i = 0; i < 4; ++i) r.Process({PointIndex()}, i, out);

  bool saw_ended = false;
  bool saw_began = false;
  for (int i = 0; i < 10; ++i) {
    r.Process({TwoFingers()}, i, out);
    const int ended = CountPhase(out, Phase::Ended);
    const int began = CountPhase(out, Phase::Began);
    EXPECT_FALSE(ended > 0 && began > 0)
        << "frame " << i << " carried both a release and a press";
    if (ended > 0) {
      saw_ended = true;
      EXPECT_FALSE(saw_began) << "the new gesture began before the old ended";
    }
    if (began > 0) saw_began = true;
  }
  EXPECT_TRUE(saw_ended);
  EXPECT_TRUE(saw_began);
}

// The count is configuration, not a constant hidden in the state machine.
TEST(GestureRecognizer, TakeoverHonoursTheConfiguredConfirmationCount) {
  GestureConfig c = GestureConfig::Default();
  c.steal_confirm_frames = 5;
  GestureRecognizer r(c);
  std::vector<GestureEvent> out;

  for (int i = 0; i < 4; ++i) r.Process({PointIndex()}, i, out);

  // Four winning frames are not enough: still held, nothing released.
  for (int i = 0; i < 4; ++i) {
    r.Process({TwoFingers()}, i, out);
    ASSERT_EQ(out.size(), 1u) << "frame " << i;
    EXPECT_EQ(out[0].kind, GestureKind::PointIndex);
    EXPECT_EQ(out[0].phase, Phase::Held) << "frame " << i;
  }

  // The fifth is.
  r.Process({TwoFingers()}, 4, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, GestureKind::PointIndex);
  EXPECT_EQ(out[0].phase, Phase::Ended);
}

// --- the plan's nine behaviours ------------------------------------------

// Spec defect 5: the Python latch reset every frame, so a held pinch fired a
// click on every one of them.
TEST(GestureRecognizer, OneShotGestureFiresExactlyOnceWhileHeld) {
  GestureRecognizer r(GestureConfig::Default());
  const std::vector<GestureEvent> ev = RunPose(r, PinchAt(0.05f), 30);

  EXPECT_EQ(CountPhase(ev, Phase::Began), 1);
  EXPECT_EQ(CountPhase(ev, Phase::Held), 29);
  EXPECT_EQ(CountPhase(ev, Phase::Ended), 0);
  EXPECT_EQ(ev.front().phase, Phase::Began);
  for (const auto& e : ev) EXPECT_EQ(e.kind, GestureKind::Pinch);
}

TEST(GestureRecognizer, ReleasingAndRepeatingFiresAgain) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  const auto feed = [&](const HandObservation& pose, int frames) {
    for (int i = 0; i < frames; ++i) {
      r.Process({pose}, 0, out);
      all.insert(all.end(), out.begin(), out.end());
    }
  };

  feed(PinchAt(0.05f), 5);   // Closed.
  feed(PinchAt(0.90f), 5);   // Released, well past the exit threshold.
  feed(PinchAt(0.05f), 5);   // Closed again.

  EXPECT_EQ(CountPhase(all, Phase::Began), 2);
  EXPECT_EQ(CountPhase(all, Phase::Ended), 1);
}

// Spec defect 4: shared state kept firing for a hand that had left frame.
TEST(GestureRecognizer, LosingAHandEmitsEndedAndClearsState) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;

  for (int i = 0; i < 5; ++i) r.Process({PinchAt(0.05f)}, 0, out);

  // The hand vanishes.
  r.Process({}, 0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].phase, Phase::Ended);
  EXPECT_EQ(out[0].kind, GestureKind::Pinch);

  // And stays gone: no further events for a track that no longer exists.
  for (int i = 0; i < 5; ++i) {
    r.Process({}, 0, out);
    EXPECT_TRUE(out.empty());
  }
}

// The owner's target behaviour, and the reason two hands were chosen: hold an
// arrow key with one hand while the other works the cursor.
TEST(GestureRecognizer, PerHandStateIsIndependent) {
  GestureRecognizer r(GestureConfig::Default());
  const HandObservation left = Fist(7, {0.25f, 0.8f});
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  const auto feed = [&](const HandObservation& right) {
    r.Process({left, right}, 0, out);
    all.insert(all.end(), out.begin(), out.end());
  };

  for (int i = 0; i < 4; ++i) feed(PinchAt(0.05f, 9, {0.75f, 0.8f}));
  for (int i = 0; i < 4; ++i) feed(PinchAt(0.90f, 9, {0.75f, 0.8f}));
  for (int i = 0; i < 4; ++i) feed(PinchAt(0.05f, 9, {0.75f, 0.8f}));
  for (int i = 0; i < 4; ++i) feed(PinchAt(0.90f, 9, {0.75f, 0.8f}));

  // The right hand pinched and released twice.
  EXPECT_EQ(CountFor(all, 9, Phase::Began), 2);
  EXPECT_EQ(CountFor(all, 9, Phase::Ended), 2);

  // The left hand's fist began once and was never interrupted: 16 frames,
  // one Began and fifteen Held, no Ended anywhere.
  EXPECT_EQ(CountFor(all, 7, Phase::Began), 1);
  EXPECT_EQ(CountFor(all, 7, Phase::Held), 15);
  EXPECT_EQ(CountFor(all, 7, Phase::Ended), 0);
  for (const auto& e : all) {
    if (e.track_id == 7) EXPECT_EQ(e.kind, GestureKind::Fist);
  }
}

// Review focus 3. A click that fires thirty times a second is the failure
// mode; without the enter/exit gap this emits roughly one Began per two
// frames.
TEST(GestureRecognizer, HysteresisStopsChatterAtTheThreshold) {
  const GestureConfig c = GestureConfig::Default();
  GestureRecognizer r(c);
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  // Straddle the enter threshold by a hair, 100 frames.
  const float enter =
      c.thresholds[static_cast<std::size_t>(GestureKind::Pinch)].enter;
  const float at_enter = (1.0f - enter) * c.pinch_span;
  for (int i = 0; i < 100; ++i) {
    const float ratio = at_enter + (i % 2 == 0 ? -0.01f : 0.01f);
    r.Process({PinchAt(ratio)}, i, out);
    all.insert(all.end(), out.begin(), out.end());
  }

  EXPECT_EQ(CountPhase(all, Phase::Began), 1);
  EXPECT_EQ(CountPhase(all, Phase::Ended), 0);
}

// Spec defect 2: absence of a hand is a state, not an error.
TEST(GestureRecognizer, EmptyHandVectorIsNormalAndEmitsNothing) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;
  for (int i = 0; i < 10; ++i) {
    r.Process({}, i * 1000, out);
    EXPECT_TRUE(out.empty());
  }
}

// Spec defect 3 is an unbound variable on exactly this case. Phase 1 measured
// handedness confidence as low as 0.575, so Unknown is a live value.
TEST(GestureRecognizer, UnknownHandednessIsNotTreatedAsLeftOrRight) {
  GestureRecognizer r(GestureConfig::Default());
  HandObservation o = PinchAt(0.05f);
  o.handedness = Handedness::Unknown;
  o.handedness_score = 0.4f;

  const std::vector<GestureEvent> ev = RunPose(r, o, 5);

  // The gesture is still recognized -- an unlabelled hand is a hand.
  ASSERT_FALSE(ev.empty());
  EXPECT_EQ(CountPhase(ev, Phase::Began), 1);
  // And it is not quietly promoted to a side.
  for (const auto& e : ev) EXPECT_EQ(e.handedness, Handedness::Unknown);
}

// Review focus 4. The label is computed per crop and flips; identity follows
// track_id, so a flip must not end one gesture and start another, and must
// not hand the event to the other hand's binding.
TEST(GestureRecognizer, HandednessFlipMidTrackDoesNotRebindTheGesture) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> all;
  std::vector<GestureEvent> out;

  HandObservation o = PinchAt(0.05f);
  o.handedness = Handedness::Right;
  for (int i = 0; i < 5; ++i) {
    r.Process({o}, 0, out);
    all.insert(all.end(), out.begin(), out.end());
  }

  o.handedness = Handedness::Left;  // The model changed its mind.
  for (int i = 0; i < 5; ++i) {
    r.Process({o}, 0, out);
    all.insert(all.end(), out.begin(), out.end());
  }

  EXPECT_EQ(CountPhase(all, Phase::Began), 1);
  EXPECT_EQ(CountPhase(all, Phase::Ended), 0);
  for (const auto& e : all) {
    EXPECT_EQ(e.handedness, Handedness::Right)
        << "handedness is latched at Began, not re-read per frame";
  }
}

TEST(GestureRecognizer, TimestampsArePassedThroughNotRead) {
  GestureRecognizer r(GestureConfig::Default());
  const HandObservation pose = PinchAt(0.05f);
  std::vector<GestureEvent> out;

  // Deliberately non-monotonic, including a repeat and a step backwards. The
  // recognizer must neither hang nor reorder nor correct these.
  const std::int64_t stamps[] = {5000, 1000, 1000, -200, 9000};
  for (std::int64_t t : stamps) {
    r.Process({pose}, t, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].timestamp_us, t);
  }
}

// --- position, delta, and buffer contract --------------------------------

TEST(GestureRecognizer, DeltaIsZeroOnBeganThenTracksPalmMovement) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;

  r.Process({PinchAt(0.05f, 1, {0.40f, 0.80f})}, 0, out);
  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].phase, Phase::Began);
  EXPECT_FLOAT_EQ(out[0].delta.x, 0.0f);
  EXPECT_FLOAT_EQ(out[0].delta.y, 0.0f);
  const Point2 first = out[0].position;

  r.Process({PinchAt(0.05f, 1, {0.50f, 0.80f})}, 0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].phase, Phase::Held);
  EXPECT_NEAR(out[0].delta.x, 0.10f, 1e-4f);
  EXPECT_NEAR(out[0].delta.y, 0.0f, 1e-4f);
  EXPECT_NEAR(out[0].position.x, first.x + 0.10f, 1e-4f);
}

TEST(GestureRecognizer, PositionIsThePalmCentreNotTheWrist) {
  GestureRecognizer r(GestureConfig::Default());
  const HandObservation pose = PinchAt(0.05f, 1, {0.40f, 0.80f});
  std::vector<GestureEvent> out;
  r.Process({pose}, 0, out);

  ASSERT_EQ(out.size(), 1u);
  const Point2 expected = hc::ComputeFeatures(pose).palm_center;
  EXPECT_FLOAT_EQ(out[0].position.x, expected.x);
  EXPECT_FLOAT_EQ(out[0].position.y, expected.y);
}

TEST(GestureRecognizer, ProcessClearsTheOutputBuffer) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out(4);  // Stale content from a previous frame.
  r.Process({}, 0, out);
  EXPECT_TRUE(out.empty());
}

// --- invalidation ---------------------------------------------------------

// InvalidateTrack is the silent path, and the header says so: it strands a
// held key. The test pins that it emits nothing, so the behaviour is a
// decision on record rather than a surprise.
TEST(GestureRecognizer, InvalidateTrackForgetsSilently) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;
  for (int i = 0; i < 3; ++i) r.Process({PinchAt(0.05f)}, 0, out);

  r.InvalidateTrack(1);
  r.Process({}, 0, out);
  EXPECT_TRUE(out.empty()) << "no Ended: the track was already forgotten";

  // The slot is reusable, and the gesture starts fresh rather than resuming.
  r.Process({PinchAt(0.05f)}, 0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].phase, Phase::Began);
}

TEST(GestureRecognizer, InvalidatingAnUnknownTrackIsHarmless) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;
  r.InvalidateTrack(12345);
  r.Process({PinchAt(0.05f)}, 0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].phase, Phase::Began);
}

TEST(GestureRecognizer, ResetDropsEveryTrack) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<GestureEvent> out;
  r.Process({Fist(1, {0.25f, 0.8f}), PinchAt(0.05f, 2, {0.75f, 0.8f})}, 0,
            out);
  ASSERT_EQ(out.size(), 2u);

  r.Reset();
  r.Process({}, 0, out);
  EXPECT_TRUE(out.empty());
}

// --- capacity -------------------------------------------------------------

// The ceiling must drop the extra track, not evict a live one: a recognizer
// that recycled the oldest slot would silently retarget a held gesture onto a
// different hand.
TEST(GestureRecognizer, ExcessTracksAreIgnoredNotRecycled) {
  GestureRecognizer r(GestureConfig::Default());
  std::vector<HandObservation> hands;
  for (std::size_t i = 0; i < hc::kMaxGestureTracks + 4; ++i) {
    hands.push_back(PinchAt(0.05f, static_cast<std::uint32_t>(i + 1),
                            {0.5f, 0.8f}));
  }

  std::vector<GestureEvent> out;
  r.Process(hands, 0, out);
  EXPECT_EQ(out.size(), hc::kMaxGestureTracks);

  // The tracks that did get slots keep them across frames.
  r.Process(hands, 0, out);
  EXPECT_EQ(out.size(), hc::kMaxGestureTracks);
  EXPECT_EQ(CountPhase(out, Phase::Held), static_cast<int>(hc::kMaxGestureTracks));
}

// --- the published spelling of a gesture ---------------------------------

TEST(GestureNames, PhaseNamesMatchTheBindingsFileColumn) {
  EXPECT_STREQ(hc::GesturePhaseName(Phase::Began), "began");
  EXPECT_STREQ(hc::GesturePhaseName(Phase::Held), "held");
  EXPECT_STREQ(hc::GesturePhaseName(Phase::Ended), "ended");
}

// The overlay HUD's acceptance criterion, and the owner's target behaviour
// read off the screen: with two hands in frame, BOTH gestures are named, each
// one its own. A single shared caption would have been the obvious bug.
TEST(GestureNames, TwoHandsAreDescribedIndependently) {
  const std::string right =
      hc::DescribeGesture(GestureKind::PointIndex, Phase::Held);
  const std::string left =
      hc::DescribeGesture(GestureKind::ThumbUp, Phase::Began);

  EXPECT_EQ(right, "point_index held");
  EXPECT_EQ(left, "thumb_up began");
  EXPECT_NE(right, left);
}

// Every kind and phase produces a non-empty description naming its own kind,
// so a kind added to the enum cannot reach a HUD as an empty caption.
TEST(GestureNames, EveryKindAndPhaseDescribesItself) {
  const Phase phases[] = {Phase::Began, Phase::Held, Phase::Ended};
  for (std::size_t i = 0; i < hc::kGestureKindCount; ++i) {
    const GestureKind kind = static_cast<GestureKind>(i);
    for (const Phase phase : phases) {
      const std::string text = hc::DescribeGesture(kind, phase);
      EXPECT_NE(text.find(hc::GestureKindName(kind)), std::string::npos)
          << text;
      EXPECT_NE(text.find(hc::GesturePhaseName(phase)), std::string::npos)
          << text;
    }
  }
}
