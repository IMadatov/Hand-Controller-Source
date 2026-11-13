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

// A single tracked hand slot. `roi` is the RotatedRect the tracker is
// currently following for this hand, in frame-normalized space (the same
// convention as TransformRect/CropToFrame use elsewhere). `active` marks
// whether the slot currently holds a hand; inactive slots are never
// considered for matching.
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
// `aspect` is frame width / frame height and MUST be the real frame's
// aspect; it is not a free choice. A TrackSlot's roi is rotated --
// RoiFromPalm derives the rotation from palm keypoints, so it is
// essentially never 0 -- and rotating a rect at the wrong aspect does not
// merely rescale its axis-aligned hull, it produces a geometrically
// different hull, shifting the IoU by roughly 10% on a 16:9 frame. Only
// slot.roi passes through AxisAlignedBounds; d.box is already axis-aligned
// and carries no aspect dependence, so there is no "both boxes scale
// together" invariance available to excuse a hardcoded aspect here.
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
std::vector<int> AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                                         const std::vector<PalmDetection>& dets,
                                         float aspect, float iou_threshold);

// Returns a monotonically increasing track id, starting at 1. Never returns
// 0 and never reuses an id (process-global, thread-safe counter). 0 is
// reserved to mean "no id", matching TrackSlot::id's default.
std::uint32_t NextTrackId();

}  // namespace hc
