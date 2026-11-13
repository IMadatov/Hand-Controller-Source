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
                    Handedness handedness = Handedness::Unknown,
                    float rotation_rad = 0.0f) {
  TrackSlot s{};
  s.id = id;
  s.roi = RotatedRect{Point2{cx, cy}, w, h, rotation_rad};
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
  int idx = MatchSlotForDetection(slots, d, /*aspect=*/1.0f, kDefaultAssociationIou);
  EXPECT_EQ(idx, 0);
}

TEST(Association, DisjointDetectionIsUnmatched) {
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.1f, 0.1f, 0.1f, 0.1f)};
  PalmDetection d = MakeDetection(0.8f, 0.8f, 0.1f, 0.1f);
  int idx = MatchSlotForDetection(slots, d, /*aspect=*/1.0f, kDefaultAssociationIou);
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
      AssignDetectionsToSlots(slots, dets, /*aspect=*/1.0f, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], 1);  // detection near right slot -> slot index 1
  EXPECT_EQ(result[1], 0);  // detection near left slot -> slot index 0
}

TEST(Association, CrossingHandsDoNotSwapSlots) {  // Review Focus 3
  // Slots at 0.45 and 0.55 with detections at 0.46 and 0.54: each detection
  // takes the nearer slot. Boxes are sized (0.2 wide) so that BOTH cross
  // pairs (slot0/det1 and slot1/det0) have IoU ABOVE kDefaultAssociationIou
  // (0.3) -- not merely nonzero. AssignDetectionsToSlots only ever considers
  // a pair whose IoU exceeds iou_threshold, so a cross pair with
  // nonzero-but-below-threshold IoU (as a 0.14-wide box would give here,
  // ~0.217) is never a candidate at all and the test would be vacuous: the
  // greedy selection could be broken and this geometry would still pass,
  // because the assignment would be forced regardless. At 0.2 wide, all four
  // pairs clear the threshold, so the descending-IoU selection and its
  // tie-break are genuinely exercised.
  //
  // Pairwise IoUs (all boxes 0.2 x 0.2):
  //   slot0 (0.45) vs det0 (0.46): centers 0.01 apart  -> IoU ~0.9048
  //   slot0 (0.45) vs det1 (0.54): centers 0.09 apart  -> IoU ~0.3793
  //   slot1 (0.55) vs det0 (0.46): centers 0.09 apart  -> IoU ~0.3793
  //   slot1 (0.55) vs det1 (0.54): centers 0.01 apart  -> IoU ~0.9048
  // The near pairs strictly dominate the cross pairs, so the correct greedy
  // assignment is unambiguous, but all four pairs are live candidates, so
  // selecting by ascending instead of descending IoU takes a cross pair
  // first and corrupts the result.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.45f, 0.5f, 0.2f, 0.2f),
                                   MakeSlot(2, 0.55f, 0.5f, 0.2f, 0.2f)};
  std::vector<PalmDetection> dets_fwd = {
      MakeDetection(0.46f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f),
      MakeDetection(0.54f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f)};

  std::vector<int> result_fwd =
      AssignDetectionsToSlots(slots, dets_fwd, /*aspect=*/1.0f, kDefaultAssociationIou);
  ASSERT_EQ(result_fwd.size(), 2u);
  EXPECT_EQ(result_fwd[0], 0);  // det0 (0.46) -> slot0 (0.45)
  EXPECT_EQ(result_fwd[1], 1);  // det1 (0.54) -> slot1 (0.55)

  // Reversed detection vector: same physical detections, swapped order.
  // The result must track the physical correspondence, not input order.
  std::vector<PalmDetection> dets_rev = {dets_fwd[1], dets_fwd[0]};
  std::vector<int> result_rev =
      AssignDetectionsToSlots(slots, dets_rev, /*aspect=*/1.0f, kDefaultAssociationIou);
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
      AssignDetectionsToSlots(slots, dets, /*aspect=*/1.0f, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  int matched_count = (result[0] != -1 ? 1 : 0) + (result[1] != -1 ? 1 : 0);
  EXPECT_EQ(matched_count, 1);
  // Whichever matched, it must be slot 0 (the only slot).
  for (int r : result) {
    if (r != -1) EXPECT_EQ(r, 0);
  }
}

TEST(Association, RotatedSlotUsesTheFrameAspectAtBothCallSites) {
  // Review fix round 1, item (2): AxisAlignedBounds is well covered on its
  // own, but its two real call sites (MatchSlotForDetection and
  // AssignDetectionsToSlots) were previously only ever exercised with
  // rotation == 0, which is exactly the axis along which a hardcoded
  // aspect is invisible.
  //
  // CORRECTED (review fix round 2): this comment used to claim the rotated
  // case was the NORMAL case, on the grounds that a real TrackSlot::roi
  // comes from RoiFromPalm. It does not. The only production caller --
  // hand_tracker_onnx.cpp -- stores an UNROTATED tight box there,
  // precisely because association.hpp requires an extent commensurable
  // with a PalmDetection::box, which RoiFromPalm's inflated rect is not
  // (measured: IoU 0.074 for the true pair, against a 0.3 gate).
  //
  // The test is still load-bearing, and is the ONLY thing pinning the
  // aspect parameter at either call site: rotation is permitted by the API
  // -- AxisAlignedBounds exists for no other reason -- and the moment a
  // caller stores a rotated rect the aspect stops cancelling and starts
  // changing match decisions, as the numbers below show.
  //
  // Geometry: slot roi centered at (0.5, 0.5), w 0.25, h 0.15, rotated 45
  // degrees, on a 16:9 frame. Detection is an axis-aligned 0.22 box centered
  // at (0.58, 0.5). The rotated hull differs in SHAPE, not just scale,
  // between the two aspects:
  //   AxisAlignedBounds(roi, 16/9) -> w 0.2364, h 0.4203 -> IoU 0.2831
  //   AxisAlignedBounds(roi, 1.0)  -> w 0.2828, h 0.2828 -> IoU 0.4159
  // A threshold of 0.35 sits between them with ~0.067 of margin on each
  // side, so the two aspects give OPPOSITE match decisions: with the real
  // frame aspect the detection does not clear the bar, with a hardcoded
  // 1.0 it does.
  constexpr float kAspect = 16.0f / 9.0f;
  constexpr float kThreshold = 0.35f;
  std::vector<TrackSlot> slots = {
      MakeSlot(1, 0.5f, 0.5f, 0.25f, 0.15f, /*active=*/true,
               Handedness::Unknown, /*rotation_rad=*/kPi / 4.0f)};
  PalmDetection d = MakeDetection(0.58f - 0.11f, 0.5f - 0.11f, 0.22f, 0.22f);
  std::vector<PalmDetection> dets = {d};

  // Call site 1: MatchSlotForDetection.
  EXPECT_EQ(MatchSlotForDetection(slots, d, kAspect, kThreshold), -1)
      << "at the true 16:9 aspect the rotated hull gives IoU 0.2831, below "
         "the 0.35 threshold";
  EXPECT_EQ(MatchSlotForDetection(slots, d, 1.0f, kThreshold), 0)
      << "at aspect 1 the same rotated hull gives IoU 0.4159, above it";

  // Call site 2: AssignDetectionsToSlots.
  std::vector<int> at_aspect =
      AssignDetectionsToSlots(slots, dets, kAspect, kThreshold);
  ASSERT_EQ(at_aspect.size(), 1u);
  EXPECT_EQ(at_aspect[0], -1);

  std::vector<int> at_unit =
      AssignDetectionsToSlots(slots, dets, 1.0f, kThreshold);
  ASSERT_EQ(at_unit.size(), 1u);
  EXPECT_EQ(at_unit[0], 0);
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
      AssignDetectionsToSlots(slots, dets, /*aspect=*/1.0f, kDefaultAssociationIou);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], 0);
  EXPECT_EQ(result[1], 1);
}

TEST(Association, SlotMatchedToOneDetectionIsNotReusedByAnother) {
  // The mirror of the test above, and the only test that can catch a dropped
  // slot_consumed check. OneSlotTakesOnlyOneDetection cannot: with a single
  // slot, max_pairs = min(n_slots, n_dets) = 1 bounds the greedy loop to one
  // iteration, so the loop exits before slot_consumed is ever read a second
  // time. Two slots and two detections are the minimum that forces a second
  // iteration.
  //
  // slot0 outscores slot1 on BOTH detections, so if its consumption is not
  // tracked it wins det1 as well and det1 never reaches slot1 -- the only
  // slot it overlaps at all.
  //
  // Pairwise IoUs (all boxes 0.2 x 0.2, aspect 1, same y): slot0-det0
  // 0.8182, slot0-det1 0.4286, slot1-det0 0.0 (disjoint), slot1-det1 0.3333.
  // Correct greedy: take (slot0,det0) at 0.8182, then slot0 is spent and the
  // best survivor is (slot1,det1) at 0.3333, which clears the 0.3 threshold.
  // Without slot_consumed the second pick is (slot0,det1) at 0.4286 instead.
  std::vector<TrackSlot> slots = {MakeSlot(1, 0.52f, 0.5f, 0.2f, 0.2f),
                                   MakeSlot(2, 0.70f, 0.5f, 0.2f, 0.2f)};
  std::vector<PalmDetection> dets = {
      MakeDetection(0.50f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f),
      MakeDetection(0.60f - 0.1f, 0.5f - 0.1f, 0.2f, 0.2f)};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, /*aspect=*/1.0f, kDefaultAssociationIou);
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
  int idx = MatchSlotForDetection(slots, d, /*aspect=*/1.0f, kDefaultAssociationIou);
  EXPECT_EQ(idx, 0);
}

TEST(Association, InactiveSlotsAreNeverMatched) {
  std::vector<TrackSlot> slots = {
      MakeSlot(1, 0.5f, 0.5f, 0.3f, 0.3f, /*active=*/false)};
  PalmDetection d = MakeDetection(0.36f, 0.36f, 0.3f, 0.3f);
  int idx = MatchSlotForDetection(slots, d, /*aspect=*/1.0f, kDefaultAssociationIou);
  EXPECT_EQ(idx, -1);

  std::vector<PalmDetection> dets = {d};
  std::vector<int> result =
      AssignDetectionsToSlots(slots, dets, /*aspect=*/1.0f, kDefaultAssociationIou);
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
