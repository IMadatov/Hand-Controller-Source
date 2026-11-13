#include "hc/hand_tracker_onnx.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_image.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/metrics.hpp"

using hc::Handedness;
using hc::HandObservation;
using hc::HandTrackerOnnx;
using hc::Metrics;
using hc::Stage;
using hc_test::BlackLike;
using hc_test::DefaultTestConfig;
using hc_test::LoadRawFixture;
using hc_test::MirrorX;
using hc_test::OwnedImage;
using hc_test::ShiftX;
using hc_test::SideBySide;

namespace {

// Ruling T10-A: the side-by-side composite could have defeated its own
// detector. The palm detector letterboxes the WHOLE frame into a 192x192
// square, so doubling the frame width halves every hand's scale in that
// square. The actual, measured palm scores on these composites (decoded with
// the pinned min_score of 0.5 from cpp/docs/models.md) are:
//
//   single 640x480 fixture             : 0.9364            (1 detection)
//   MirrorX(fixture), 640x480          : 0.9104            (1 detection)
//   SideBySide(f, MirrorX(f)) 1280x480 : 0.8932, 0.8799    (2 detections)
//   SideBySide(f, black)      1280x480 : 0.9551            (1 detection)
//   three-wide 1920x480                : 0.8784, 0.8784, 0.8185 (3)
//
// Every hand clears 0.5 with a wide margin, so no geometry change was needed
// and none was made. If a future fixture change drops a score below the gate,
// the fix is the composite's geometry (the composite is this test file's own
// choice) -- never `min_score`, which is a model parameter, and never a
// special case in the tracker.
constexpr int kHalfW = 640;
constexpr int kHalfH = 480;

// The composite's two hands sit one per half, so "which half is this
// observation in" is a usable identity check that needs no ground truth about
// the fixture's content. Uses the mean of all 21 landmarks rather than a
// single point so one stray landmark cannot flip the answer.
float MeanX(const HandObservation& o) {
  float sum = 0.0f;
  for (const auto& p : o.landmarks) sum += p.x;
  return sum / static_cast<float>(o.landmarks.size());
}

bool InLeftHalf(const HandObservation& o) { return MeanX(o) < 0.5f; }

std::set<std::uint32_t> IdsOf(const std::vector<HandObservation>& obs) {
  std::set<std::uint32_t> ids;
  for (const auto& o : obs) ids.insert(o.track_id);
  return ids;
}

// Returns the single observation whose mean landmark x falls in the requested
// half, or fails the calling test if there is not exactly one.
const HandObservation& InHalf(const std::vector<HandObservation>& obs,
                              bool left) {
  const HandObservation* found = nullptr;
  int count = 0;
  for (const auto& o : obs) {
    if (InLeftHalf(o) == left) {
      found = &o;
      ++count;
    }
  }
  EXPECT_EQ(count, 1) << "expected exactly one observation in the "
                      << (left ? "left" : "right") << " half, got " << count;
  if (found == nullptr) {
    static const HandObservation kEmpty{};
    return kEmpty;
  }
  return *found;
}

// The composite the two-hand tests run on: the fixture beside its own mirror,
// 1280x480.
OwnedImage TwoHandComposite(const OwnedImage& f) {
  return SideBySide(f, MirrorX(f));
}

}  // namespace

TEST(HandTrackerTwo, FindsTwoHandsInSideBySideFixture) {
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto composite = TwoHandComposite(f);
  ASSERT_EQ(composite.view.width, 2 * kHalfW);
  ASSERT_EQ(composite.view.height, kHalfH);

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;
  t.Process(composite.view, 0, obs);

  ASSERT_EQ(obs.size(), 2u);
  EXPECT_NE(obs[0].track_id, obs[1].track_id);
  EXPECT_NE(obs[0].track_id, 0u);
  EXPECT_NE(obs[1].track_id, 0u);

  // Distinct ids alone do NOT prove two hands were found: a tracker that
  // seeded both slots from the same detection would also report two ids.
  // Requiring one observation per half pins that the two slots really are on
  // two different hands.
  EXPECT_NE(InLeftHalf(obs[0]), InLeftHalf(obs[1]))
      << "both observations landed in the same half of the composite";

  for (const auto& o : obs) {
    EXPECT_GT(o.presence, 0.5f);
    // Ruling T10-B / T8-E: the Left-vs-Right label order is still NOT
    // DETERMINED in cpp/docs/models.md, so this asserts only that a side was
    // assigned. The observed handedness flip under MirrorX is recorded in
    // models.md, not asserted as an absolute side here.
    EXPECT_NE(o.handedness, Handedness::Unknown);
    for (const auto& p : o.landmarks) {
      EXPECT_GE(p.x, 0.0f);
      EXPECT_LE(p.x, 1.0f);
      EXPECT_GE(p.y, 0.0f);
      EXPECT_LE(p.y, 1.0f);
    }
  }
}

TEST(HandTrackerTwo, TrackIdsAreStableAcrossFrames) {
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto composite = TwoHandComposite(f);

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;

  std::set<std::uint32_t> first_ids;
  std::uint32_t left_id = 0, right_id = 0;
  for (int i = 0; i < 5; ++i) {
    t.Process(composite.view, i, obs);
    ASSERT_EQ(obs.size(), 2u) << "lost a track on frame " << i;
    if (i == 0) {
      first_ids = IdsOf(obs);
      left_id = InHalf(obs, /*left=*/true).track_id;
      right_id = InHalf(obs, /*left=*/false).track_id;
      ASSERT_EQ(first_ids.size(), 2u);
    } else {
      EXPECT_EQ(IdsOf(obs), first_ids) << "track id set changed on frame " << i;
      // A tracker that kept the same two ids but swapped which hand they
      // point at would satisfy the set comparison above while being useless
      // to any consumer keyed by track_id. Pin the id-to-hand binding, not
      // just the id set.
      EXPECT_EQ(InHalf(obs, /*left=*/true).track_id, left_id)
          << "left hand's id changed on frame " << i;
      EXPECT_EQ(InHalf(obs, /*left=*/false).track_id, right_id)
          << "right hand's id changed on frame " << i;
    }
  }

  // Both slots stay active the whole time, so the detector never re-fires.
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
}

TEST(HandTrackerTwo, LandmarkRunsOncePerTrackedHand) {
  // The doubled inference cost of a second hand, measured rather than
  // assumed: 5 frames x 2 tracked hands = 10 landmark invocations, against
  // the single palm detection that seeded them both.
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto composite = TwoHandComposite(f);

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;
  for (int i = 0; i < 5; ++i) {
    t.Process(composite.view, i, obs);
    ASSERT_EQ(obs.size(), 2u) << "lost a track on frame " << i;
  }
  EXPECT_EQ(m.Get(Stage::Landmark).calls, 10u);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
}

TEST(HandTrackerTwo, LosingOneHandKeepsTheOther) {
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto composite = TwoHandComposite(f);
  // Blanking the right half rather than shrinking the frame keeps the
  // geometry -- and therefore the letterbox, and therefore the surviving
  // hand's scale in the detector input -- identical between the two frames.
  const auto left_only = SideBySide(f, BlackLike(f));

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;

  t.Process(composite.view, 0, obs);
  ASSERT_EQ(obs.size(), 2u);
  const std::uint32_t left_id = InHalf(obs, /*left=*/true).track_id;
  const std::uint32_t right_id = InHalf(obs, /*left=*/false).track_id;
  ASSERT_NE(left_id, right_id);
  ASSERT_EQ(m.Get(Stage::PalmDetect).calls, 1u);

  t.Process(left_only.view, 1, obs);
  ASSERT_EQ(obs.size(), 1u) << "the right hand's slot should have dropped out";
  // The SURVIVOR must be the left hand, with its original id. Checking the id
  // alone would pass a tracker that deactivated the wrong slot and then
  // re-seeded; checking the half alone would pass one that re-detected with a
  // fresh id.
  EXPECT_TRUE(InLeftHalf(obs[0])) << "the wrong hand survived";
  EXPECT_EQ(obs[0].track_id, left_id);
  EXPECT_NE(obs[0].track_id, right_id);

  // Ruling T10-F: the detector's decision is made from the state at frame
  // ENTRY (Task 8's ordering, which HandTracker.ReDetectsAfterHandDisappears
  // already pins). Both slots were still active when frame 1 began, so frame
  // 1 does NOT re-run the detector; the deactivation fires it on frame 2.
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);

  // A THIRD frame, and it is load-bearing -- frame 1 alone cannot see a
  // botched deactivation. Slots are processed in order and each pushes its
  // observation before the next one runs, so by the time the right slot dies
  // the left slot has ALREADY emitted. Mutations that deactivate the wrong
  // slot (slots_[0] instead of the current one) or deactivate every slot
  // rather than just this one therefore leave frame 1 looking perfectly
  // correct: one observation, left half, right id. The damage only becomes
  // visible on the next frame, when the wrongly-killed left slot gets
  // re-seeded by the detector and comes back with a NEW id.
  t.Process(left_only.view, 2, obs);
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_TRUE(InLeftHalf(obs[0]));
  EXPECT_EQ(obs[0].track_id, left_id)
      << "the surviving hand's track was dropped and re-acquired; only the "
         "slot whose presence fell below min_presence may be deactivated";
  // Frame 2 entered with a free slot, so the detector runs exactly once more
  // (ruling T10-D: an empty slot costs a palm detection every frame), and it
  // must not have stolen the live hand into the free slot.
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
}

TEST(HandTrackerTwo, DetectorRunsOnlyForTheMissingHand) {
  // NAMING (ruling T10-C): "only for the missing hand" refers to the CALL
  // COUNT, not to spatial scoping. Step 3 runs the palm detector once over
  // the WHOLE frame whenever any slot is inactive -- it is not restricted to
  // the region where the hand went missing, and it is not run once per free
  // slot. De-duplicating the resulting detections against hands that are
  // already tracked is the association layer's job (AssignDetectionsToSlots),
  // which is exactly what this test's "the still-tracked hand keeps its id"
  // assertion exercises. Do not build per-hand detection from this name.
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto composite = TwoHandComposite(f);
  const auto left_only = SideBySide(f, BlackLike(f));

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;

  t.Process(composite.view, 0, obs);
  ASSERT_EQ(obs.size(), 2u);
  const std::uint32_t left_id = InHalf(obs, /*left=*/true).track_id;

  t.Process(left_only.view, 1, obs);
  ASSERT_EQ(obs.size(), 1u);
  ASSERT_EQ(obs[0].track_id, left_id);
  const std::uint64_t before = m.Get(Stage::PalmDetect).calls;
  ASSERT_EQ(before, 1u);

  t.Process(composite.view, 2, obs);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, before + 1u)
      << "the detector must run exactly once, over the whole frame";
  ASSERT_EQ(obs.size(), 2u);

  // The still-tracked hand keeps its id: its detection must have been matched
  // to the live slot and DISCARDED, not used to seed a second slot.
  EXPECT_EQ(InHalf(obs, /*left=*/true).track_id, left_id);
  // And the two slots must be on two different hands. Without this, dropping
  // the "discard detections matching an already-active slot" step still
  // passes: the left hand's detection would seed the free slot, giving two
  // observations and a preserved left id -- both slots stacked on one hand.
  EXPECT_NE(InLeftHalf(obs[0]), InLeftHalf(obs[1]))
      << "the re-detection stacked a second slot onto the tracked hand";
  const std::uint32_t new_right_id = InHalf(obs, /*left=*/false).track_id;
  EXPECT_NE(new_right_id, 0u);
  EXPECT_NE(new_right_id, left_id);
}

TEST(HandTrackerTwo, AssociationFollowsAMovingHand) {
  // THE TEST FOR THE PER-FRAME ASSOCIATION-RECT REFRESH.
  //
  // RunLandmarksForSlot ends with two assignments: the crop ROI for next
  // frame, and `slot.t.roi` -- the tight box the association layer compares
  // against palm detections. Deleting that second line leaves every other
  // test in this file GREEN, because every other fixture here is a STATIC
  // image: the rect seeded at detect time and the rect the landmarks would
  // refresh it to describe the same hand in the same place, so the stale
  // value scores IoU ~1 and still matches. The refresh is only load-bearing
  // once the hand MOVES, which is what ShiftX manufactures.
  //
  // MEASURED on this exact sequence (frame-normalized boxes in the 1280x480
  // composite, association gate kDefaultAssociationIou = 0.3):
  //
  //   frame 0 seed box (detection)            : [0.0601 0.3879 0.2166 0.5772]
  //   frame 1 refreshed tight landmark box    : [0.1371 0.2800 0.3008 0.6971]
  //   frame 2 detection box (hand at +240 px) : [0.2210 0.3496 0.2343 0.6243]
  //
  //   IoU(frame-1 refreshed rect, frame-2 detection) = 0.614  -> MATCHES
  //   IoU(frame-0 seed rect,      frame-2 detection) = 0.134  -> NO MATCH
  //
  // 0.314 of margin above the gate with the refresh, 0.166 below it without.
  // The shift of 120 px per frame was chosen by sweeping dx over
  // {0,40,60,80,100,120} and reading those IoUs out of the tracker, not
  // guessed: smaller shifts leave the stale rect still matching (dx=100 gives
  // 0.193, only 0.107 of margin), larger ones start costing landmark presence
  // (0.974 at dx=120 frame 2, already falling from 0.991).
  //
  // Only the left half ever holds a hand, so with max_hands = 2 the right
  // slot is permanently empty and the detector fires on EVERY frame (ruling
  // T10-D) -- which is what gets a frame-2 detection in front of the
  // association layer at all.
  constexpr int kShiftPerFrame = 120;

  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto black = BlackLike(f);
  const auto frame0 = SideBySide(f, black);
  const auto frame1 = SideBySide(ShiftX(f, kShiftPerFrame), black);
  const auto frame2 = SideBySide(ShiftX(f, 2 * kShiftPerFrame), black);

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;

  t.Process(frame0.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);
  const std::uint32_t id = obs[0].track_id;
  const float x0 = MeanX(obs[0]);

  t.Process(frame1.view, 1, obs);
  ASSERT_EQ(obs.size(), 1u) << "landmark tracking lost the hand on frame 1";
  ASSERT_EQ(obs[0].track_id, id);
  const float x1 = MeanX(obs[0]);

  t.Process(frame2.view, 2, obs);

  // THE ASSERTION. Without the refresh the frame-2 detection scores 0.134
  // against the frozen frame-0 rect, is declared unmatched, and seeds the
  // still-free second slot -- so the frame ends with TWO slots stacked on
  // one hand and a new id alongside the old one.
  ASSERT_EQ(obs.size(), 1u)
      << "the moved hand was re-detected as a second, new hand: the "
         "association rect is not being refreshed from each frame's "
         "landmarks, so it still describes where the hand was on frame 0";
  EXPECT_EQ(obs[0].track_id, id)
      << "the moved hand lost its track id";
  const float x2 = MeanX(obs[0]);

  // The hand really did move, and stayed inside the left half the whole
  // time -- otherwise "the right half is blank" would stop being true and
  // the detection count would change for an unrelated reason.
  EXPECT_GT(x1, x0 + 0.02f);
  EXPECT_GT(x2, x1 + 0.02f);
  EXPECT_LT(x2, 0.5f);

  // Three frames, each entered with the second slot free.
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 3u);
}

TEST(HandTrackerTwo, NeverExceedsMaxHands) {
  // Three hands in frame, max_hands = 2. Measured palm scores on this
  // 1920x480 composite are 0.8784 / 0.8784 / 0.8185 -- all three hands are
  // genuinely detectable, so the cap is what limits the output, not the
  // detector (ruling T10-A).
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  const auto three = SideBySide(SideBySide(f, MirrorX(f)), f);
  ASSERT_EQ(three.view.width, 3 * kHalfW);
  ASSERT_EQ(three.view.height, kHalfH);

  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/2), m);
  std::vector<HandObservation> obs;
  t.Process(three.view, 0, obs);
  EXPECT_EQ(obs.size(), 2u);

  // Two distinct hands, not one hand reported twice.
  ASSERT_EQ(obs.size(), 2u);
  EXPECT_NE(obs[0].track_id, obs[1].track_id);

  // A second frame must not let a third slot appear either: the cap is a
  // standing invariant, not a first-frame one.
  t.Process(three.view, 1, obs);
  EXPECT_EQ(obs.size(), 2u);
}

TEST(HandTrackerTwo, MaxHandsOneStillWorks) {
  // Regression guard (ruling T10-G): the slot refactor must not change the
  // single-hand behaviour Task 8 pinned. Same assertions as
  // HandTracker.SkipsPalmDetectorWhileTracking, re-stated here so this file
  // fails on its own if the refactor regresses it -- and, critically, run on
  // the TWO-hand composite as well as the single fixture, so that max_hands
  // = 1 is shown to cap a frame that genuinely contains two detectable hands.
  Metrics m;
  const auto f = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs;
  for (int i = 0; i < 5; ++i) {
    t.Process(f.view, i, obs);
    ASSERT_EQ(obs.size(), 1u) << "lost the track on frame " << i;
  }
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 1u);
  EXPECT_EQ(m.Get(Stage::Landmark).calls, 5u);

  Metrics m2;
  const auto composite = TwoHandComposite(f);
  HandTrackerOnnx t2(DefaultTestConfig(/*max_hands=*/1), m2);
  std::vector<HandObservation> obs2;
  t2.Process(composite.view, 0, obs2);
  EXPECT_EQ(obs2.size(), 1u) << "max_hands = 1 must cap a two-hand frame";
  EXPECT_EQ(m2.Get(Stage::Landmark).calls, 1u);
}
