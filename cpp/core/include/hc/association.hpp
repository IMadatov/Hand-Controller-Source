#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"
#include "hc/palm_decode.hpp"

namespace hc {

// Spatial association between already-tracked hands (TrackSlot) and newly
// decoded palm detections (PalmDetection). Pure logic: no model, no
// inference. Task 10 (two-hand tracking) is the consumer.
//
// COORDINATE SPACE (ruling T9-A): every function in this file operates
// entirely in FRAME-NORMALIZED space, i.e. the same space HandObservation's
// landmarks use, where (0,0)/(1,1) are the full camera frame's corners.
// PalmDetection::box as produced directly by DecodePalmDetections is NOT in
// this space -- it is normalized to the detector's 192x192 input square.
// Mapping a detection from detector-square space into frame-normalized space
// (via CropToFrame, as hand_tracker_onnx.cpp's PalmToFrameSpace already does
// for the single-hand path) is the caller's job, not this file's. Passing a
// detector-space PalmDetection into MatchSlotForDetection or
// AssignDetectionsToSlots directly will silently misassociate on any
// non-square frame, and will look correct on a square fixture -- so callers
// must map to frame space first.
//
// The spec is explicit that the landmark model's handedness output is
// computed per-crop and can flip frame to frame, so spatial continuity (IoU
// of the slot's ROI against the detection's box) is the primary association
// signal here. Handedness is a cross-check for a later task, not an input to
// any function below.

// A single tracked hand slot. `roi` is this hand's current extent in
// frame-normalized space (the same convention as TransformRect/CropToFrame
// use elsewhere). `active` marks whether the slot currently holds a hand;
// inactive slots are never considered for matching.
//
// `roi` MUST BE A TIGHT EXTENT, COMMENSURABLE WITH A PalmDetection::box.
// This is a hard precondition of every function below, not a style note.
// The only thing `roi` is ever used for here is IoU against a raw palm
// detection box, and IoU is bounded above by the ratio of the two areas --
// so a rect that is merely the right SHAPE but the wrong SCALE cannot match
// even a perfect detection of the very hand it is tracking.
//
// In particular, do NOT store the inflated ROI the landmark model is
// cropped from. That rect is LandmarkRoiOptions/PalmRoiOptions-derived
// (scaled ~2x and squared on the long side), and Task 10 MEASURED the
// consequence on the committed fixture: 2.0598 in area against the
// detection box's 0.1518, giving the TRUE slot/detection pair an IoU of
// 0.074 -- below kDefaultAssociationIou (0.3), so a correct match was
// arithmetically impossible and every re-detection stacked a second slot
// onto an already-tracked hand. The tight 12-point landmark bounding box
// (MediaPipe's ROI index subset, fingertips excluded) scores 0.557 for the
// same pair and 0.000 for false pairs. hand_tracker_onnx.cpp keeps the two
// rects as separate fields for exactly this reason; see TightLandmarkBox
// there.
//
// `roi` MAY carry a non-zero rotation and MUST be interpreted through
// AxisAlignedBounds (which is what the matchers do). The tracker in this
// repo happens to store an unrotated rect, but nothing here requires that
// and the `aspect` argument below is load-bearing the moment it is not.
struct TrackSlot {
  std::uint32_t id = 0;
  RotatedRect roi{};
  Handedness handedness = Handedness::Unknown;
  int misses = 0;
  bool active = false;
};

// Tracking parameter chosen in this file for greedy slot/detection
// association. This is NOT a model parameter and is NOT sourced from
// cpp/docs/models.md -- it is a tracking-policy constant, even though its
// value happens to coincide with the palm decoder's NMS threshold.
constexpr float kDefaultAssociationIou = 0.3f;

// Returns the smallest axis-aligned rectangle, in frame-normalized space,
// containing the (possibly rotated) rect `r`. `aspect` is frame width /
// frame height, the same convention SquareLong and TransformRect use: r.w is
// measured against `aspect` and r.h against 1 (the "height-unit"
// convention), so the rotation must be applied in aspect-corrected space
// before converting back, or the bounds are wrong on any non-square frame.
Rect AxisAlignedBounds(const RotatedRect& r, float aspect);

// The maximum number of slots, and the maximum number of detections, that
// AssignDetectionsToSlots considers in one call. This is a per-frame path,
// so its consumed-side bookkeeping is fixed-capacity bitmask storage rather
// than per-call heap allocation; 64 is the width of that bitmask and sits
// far above the two-hand ceiling this phase targets. Entries past the cap
// are ignored rather than silently misassociated.
constexpr std::size_t kMaxAssociationEntries = 64;

// Returns the index of the best-overlapping active slot for detection `d`
// (by IoU of AxisAlignedBounds(slot.roi, aspect) against d.box, both already
// assumed frame-normalized), or -1 if no active slot overlaps `d` with IoU >
// iou_threshold. Inactive slots are never matched. This function is
// per-detection and does not consume anything, so it cannot by itself
// enforce one-detection-per-slot across a batch -- see
// AssignDetectionsToSlots for that.
//
// `slots[i].roi` must satisfy the tight-extent precondition documented on
// TrackSlot; a slot carrying an inflated tracking crop will fail to match
// the very detection it should match.
//
// `aspect` is frame width / frame height and MUST be the real frame's
// aspect; it is not a free choice. A TrackSlot's roi MAY be rotated, and
// rotating a rect at the wrong aspect does not merely rescale its
// axis-aligned hull, it produces a geometrically different hull, shifting
// the IoU by roughly 10% on a 16:9 frame (test_association.cpp's
// RotatedSlotUsesTheFrameAspectAtBothCallSites pins a case where the two
// aspects give OPPOSITE match decisions). Only slot.roi passes through
// AxisAlignedBounds; d.box is already axis-aligned and carries no aspect
// dependence, so there is no "both boxes scale together" invariance
// available to excuse a hardcoded aspect here.
//
// At rotation exactly 0 the aspect does cancel out of AxisAlignedBounds
// (it multiplies the x half-extent then divides it back), so passing the
// wrong value is harmless for an unrotated roi -- which is the case this
// repo's only production caller happens to be in. That is an accident of
// the current caller, not a property of this API: pass the real value.
int MatchSlotForDetection(const std::vector<TrackSlot>& slots,
                          const PalmDetection& d, float aspect,
                          float iou_threshold);

// Greedily assigns each detection in `dets` to at most one active slot in
// `slots`, and each slot to at most one detection, by descending IoU over
// all (slot, detection) pairs. Ties are broken deterministically by
// (slot index, detection index) so the result never depends on input order.
// Returns a vector parallel to `dets`: result[i] is the slot index matched
// to dets[i], or -1 if dets[i] is unmatched (needs a new slot).
//
// `aspect` carries the same meaning and the same hard requirement as in
// MatchSlotForDetection above.
//
// Only the first kMaxAssociationEntries slots and the first
// kMaxAssociationEntries detections are considered; see that constant.
//
// Out-param overload: writes the same result into `out` instead of
// returning a fresh vector. `out` is assigned (not appended to), so its
// prior contents are irrelevant and only its CAPACITY is reused. This
// exists because the by-value form's one surviving allocation sits inside
// the tracker's Stage::Associate timing window, i.e. it was being charged
// to a measured number. The by-value overload is a thin wrapper around
// this one, so the two cannot drift.
void AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                             const std::vector<PalmDetection>& dets,
                             float aspect, float iou_threshold,
                             std::vector<int>& out);

std::vector<int> AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                                         const std::vector<PalmDetection>& dets,
                                         float aspect, float iou_threshold);

// Returns a monotonically increasing track id, starting at 1. Never returns
// 0 and never reuses an id (process-global, thread-safe counter). 0 is
// reserved to mean "no id", matching TrackSlot::id's default.
std::uint32_t NextTrackId();

}  // namespace hc
