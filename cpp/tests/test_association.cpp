#include "hc/association.hpp"

#include <cmath>

#include <gtest/gtest.h>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"
#include "hc/palm_decode.hpp"

namespace hc {
namespace {

constexpr float kPi = 3.14159265358979323846f;

PalmDetection MakeDetection(float x, float y, float w, float h) {
  PalmDetection d{};
  d.box = Rect{x, y, w, h};
  d.score = 1.0f;
  for (auto& kp : d.keypoints) {
    kp = Point2{x + w / 2.0f, y + h / 2.0f};
  }
  return d;
}

TrackSlot MakeSlot(std::uint32_t id, float cx, float cy, float w, float h,
                    bool active = true,
                    Handedness handedness = Handedness::Unknown) {
  TrackSlot s{};
  s.id = id;
  s.roi = RotatedRect{Point2{cx, cy}, w, h, 0.0f};
  s.handedness = handedness;
  s.active = active;
  return s;
}

TEST(Association, AxisAlignedBoundsSquareRotated45IsSqrt2Larger) {
  // A unit-aspect square rotated 45 degrees: its axis-aligned bounds grow by
  // sqrt(2) on each side (the classic "diamond" bounding box).
  RotatedRect square{Point2{0.5f, 0.5f}, 0.2f, 0.2f, kPi / 4.0f};
  Rect bounds = AxisAlignedBounds(square, 1.0f);
  const float expected_side = 0.2f * std::sqrt(2.0f);
  EXPECT_NEAR(bounds.w, expected_side, 1e-4f);
  EXPECT_NEAR(bounds.h, expected_side, 1e-4f);
  EXPECT_NEAR(bounds.x, 0.5f - expected_side / 2.0f, 1e-4f);
  EXPECT_NEAR(bounds.y, 0.5f - expected_side / 2.0f, 1e-4f);
}

TEST(Association, AxisAlignedBoundsNonUnitAspectDiffersFromUnitAspect) {
  // A rect rotated 45 degrees at a non-unit aspect must produce different
  // bounds than the same rect interpreted at aspect 1 -- proving the aspect
  // parameter is actually used, not ignored (ruling T9-B).
  RotatedRect r{Point2{0.5f, 0.5f}, 0.3f, 0.2f, kPi / 4.0f};
  Rect bounds_wide = AxisAlignedBounds(r, 16.0f / 9.0f);
  Rect bounds_unit = AxisAlignedBounds(r, 1.0f);
  EXPECT_NE(bounds_wide.w, bounds_unit.w);
  EXPECT_NE(bounds_wide.h, bounds_unit.h);
}

TEST(Association, AxisAlignedBoundsUnrotatedRectIsUnchanged) {
  RotatedRect r{Point2{0.4f, 0.6f}, 0.2f, 0.1f, 0.0f};
  Rect bounds = AxisAlignedBounds(r, 16.0f / 9.0f);
  EXPECT_NEAR(bounds.x, 0.3f, 1e-5f);
  EXPECT_NEAR(bounds.y, 0.55f, 1e-5f);
  EXPECT_NEAR(bounds.w, 0.2f, 1e-5f);
  EXPECT_NEAR(bounds.h, 0.1f, 1e-5f);
}

TEST(Association, DetectionOverlappingTrackedSlotMatchesIt) {  // Review Focus 3
  // One active slot; a detection covering nearly the same area matches it,
  // rather than being reported as a new hand. This is the "palm detector
  // returns an already-tracked hand" case the spec warns about.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.5f, 0.5f, 0.3f, 0.3f)};
  PalmDetection d = MakeDetection(0.36f, 0.36f, 0.3f, 0.3f);
  int idx = MatchSlotForDetection(slots, d, kDefaultAssociationIou);
  EXPECT_EQ(idx, 0);
}

TEST(Association, DisjointDetectionIsUnmatched) {
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.1f, 0.1f, 0.1f, 0.1f)};
  PalmDetection d = MakeDetection(0.8f, 0.8f, 0.1f, 0.1f);
  int idx = MatchSlotForDetection(slots, d, kDefaultAssociationIou);
  EXPECT_EQ(idx, -1);
}

TEST(Association, TwoHandsKeepTheirOwnSlots) {
  // Two slots, left at x~0.25 and right at x~0.75; two detections in the same
  // places assign to the matching slots, not crosswise.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.25f, 0.5f, 0.2f, 0.2f),
                                   MakeSlot(2, 0.75f, 0.5f, 0.2f, 0.2f)};
  std::vector<PalmDetection> dets = {MakeDetection(0.65f, 0.4f, 0.2f, 0.2f),
                                     MakeDetection(0.15f, 0.4f, 0.2f, 0.2f)};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], 1);  // detection near right slot -> slot index 1
  EXPECT_EQ(result[1], 0);  // detection near left slot -> slot index 0
}

TEST(Association, CrossingHandsDoNotSwapSlots) {  // Review Focus 3
  // Slots at 0.45 and 0.55 with detections at 0.46 and 0.54: each detection
  // takes the nearer slot. Boxes are sized (0.2 wide) so that BOTH cross
  // pairs (slot0/det1 and slot1/det0) have IoU ABOVE kDefaultAssociationIou
  // (0.3) -- not merely nonzero. AssignDetectionsToSlots filters pairs by
  // `iou > iou_threshold` before the sort ever runs, so a cross pair with
  // nonzero-but-below-threshold IoU (as a 0.14-wide box would give here,
  // ~0.217) never reaches the sort and the test would be vacuous: the sort
  // comparator could be broken and this geometry would still pass, because
  // greedy assignment would be forced regardless. At 0.2 wide, all four
  // pairs clear the threshold, so the sort and its tie-break are genuinely
  // exercised.
  //
  // Pairwise IoUs (all boxes 0.2 x 0.2):
  //   slot0 (0.45) vs det0 (0.46): centers 0.01 apart  -> IoU ~0.9048
  //   slot0 (0.45) vs det1 (0.54): centers 0.09 apart  -> IoU ~0.3793
  //   slot1 (0.55) vs det0 (0.46): centers 0.09 apart  -> IoU ~0.3793
  //   slot1 (0.55) vs det1 (0.54): centers 0.01 apart  -> IoU ~0.9048
  // The near pairs strictly dominate the cross pairs, so the correct greedy
  // assignment is unambiguous, but all four pairs are live candidates for
  // the sort, so a broken (e.g. reversed) comparator processes a cross pair
  // first and corrupts the result.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.45f, 0.5f, 0.2f, 0.2f),
                                   MakeSlot(2, 0.55f, 0.5f, 0.2f, 0.2f)};
  std::vector<PalmDetection> dets_fwd = {
      MakeDetection(0.46f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f),
      MakeDetection(0.54f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f)};

  std::vector<int> result_fwd =
      AssignDetectionsToSlots(slots, dets_fwd, kDefaultAssociationIou);
  ASSERT_EQ(result_fwd.size(), 2u);
  EXPECT_EQ(result_fwd[0], 0);  // det0 (0.46) -> slot0 (0.45)
  EXPECT_EQ(result_fwd[1], 1);  // det1 (0.54) -> slot1 (0.55)

  // Reversed detection vector: same physical detections, swapped order.
  // The result must track the physical correspondence, not input order.
  std::vector<PalmDetection> dets_rev = {dets_fwd[1], dets_fwd[0]};
  std::vector<int> result_rev =
      AssignDetectionsToSlots(slots, dets_rev, kDefaultAssociationIou);
  ASSERT_EQ(result_rev.size(), 2u);
  EXPECT_EQ(result_rev[0], 1);  // det1 (0.54) -> slot1 (0.55)
  EXPECT_EQ(result_rev[1], 0);  // det0 (0.46) -> slot0 (0.45)
}

TEST(Association, OneSlotTakesOnlyOneDetection) {
  // Two overlapping detections against a single slot: one matches, the other
  // returns -1 rather than both claiming the slot. Written against
  // AssignDetectionsToSlots (ruling T9-E): MatchSlotForDetection is
  // per-detection and consumes nothing, so it cannot enforce this.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.5f, 0.5f, 0.3f, 0.3f)};
  std::vector<PalmDetection> dets = {MakeDetection(0.36f, 0.36f, 0.3f, 0.3f),
                                     MakeDetection(0.37f, 0.37f, 0.3f, 0.3f)};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  int matched_count = (result[0] != -1 ? 1 : 0) + (result[1] != -1 ? 1 : 0);
  EXPECT_EQ(matched_count, 1);
  // Whichever matched, it must be slot 0 (the only slot).
  for (int r : result) {
    if (r != -1) EXPECT_EQ(r, 0);
  }
}

TEST(Association, DetectionMatchedToOneSlotIsNotReassignedToAnother) {
  // slot0 and slot1 both overlap det0 strongly (slot0 more, since it is
  // centered exactly on det0); only slot1 overlaps det1. If det0's
  // consumption were not tracked, the greedy loop would process
  // (slot0,det0) first (correct), then try (slot1,det0) next (since that
  // pair's IoU exceeds (slot1,det1)'s) and -- without a det_consumed check
  // -- wrongly claim slot1 for det0 too, leaving det1 unmatched even though
  // slot1 is the only slot it fits. This is what OneSlotTakesOnlyOneDetection
  // cannot catch (it uses a single slot, so slot_consumed alone already
  // forces the right answer).
  //
  // Pairwise IoUs: slot0-det0 1.0, slot0-det1 0.5, slot1-det0 0.875,
  // slot1-det1 0.579 (all boxes 0.3 x 0.3; slot0 center 0.50, slot1 center
  // 0.52, det0 center 0.50, det1 center 0.60).
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.50f, 0.5f, 0.3f, 0.3f),
                                   MakeSlot(2, 0.52f, 0.5f, 0.3f, 0.3f)};
  std::vector<PalmDetection> dets = {MakeDetection(0.50f - 0.15f, 0.5f - 0.15f, 0.3f, 0.3f),
                                     MakeDetection(0.60f - 0.15f, 0.5f - 0.15f, 0.3f, 0.3f)};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], 0);
  EXPECT_EQ(result[1], 1);
}

TEST(Association, HandednessDisagreementDoesNotBreakSpatialMatch) {
  // A slot marked Left and a strongly overlapping detection still match.
  // Pins the spec's rule that handedness is a cross-check, not the key:
  // MatchSlotForDetection takes no handedness argument from the detection at
  // all, so a disagreement cannot possibly suppress the match.
  std::vector<TrackSlot> slots = {
      MakeSlot(1, 0.5f, 0.5f, 0.3f, 0.3f, true, Handedness::Left)};
  PalmDetection d = MakeDetection(0.36f, 0.36f, 0.3f, 0.3f);
  int idx = MatchSlotForDetection(slots, d, kDefaultAssociationIou);
  EXPECT_EQ(idx, 0);
}

TEST(Association, InactiveSlotsAreNeverMatched) {
  std::vector<TrackSlot> slots = {
      MakeSlot(1, 0.5f, 0.5f, 0.3f, 0.3f, /*active=*/false)};
  PalmDetection d = MakeDetection(0.36f, 0.36f, 0.3f, 0.3f);
  int idx = MatchSlotForDetection(slots, d, kDefaultAssociationIou);
  EXPECT_EQ(idx, -1);

  std::vector<PalmDetection> dets = {d};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0], -1);
}

TEST(Association, TrackIdsAreNeverReused) {
  std::uint32_t a = NextTrackId();
  std::uint32_t b = NextTrackId();
  std::uint32_t c = NextTrackId();
  EXPECT_NE(a, 0u);
  EXPECT_NE(b, 0u);
  EXPECT_NE(c, 0u);
  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
}

}  // namespace
}  // namespace hc
