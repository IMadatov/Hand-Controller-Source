#include "hc/association.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace hc {

Rect AxisAlignedBounds(const RotatedRect& r, float aspect) {
  // r.w is measured against `aspect`, r.h against 1 (the "height-unit"
  // convention documented in geometry.hpp and used by SquareLong /
  // TransformRect). Rotation must happen in a space where both axes share a
  // unit, so convert to aspect-corrected ("pixel-comparable") units first:
  // w_px = r.w * aspect, h_px = r.h. Compute the four rotated corners in
  // that space, take their axis-aligned bounds, then convert back by
  // dividing x extents by `aspect`.
  const float half_w_px = (r.w * aspect) * 0.5f;
  const float half_h_px = r.h * 0.5f;

  const float cos_r = std::cos(r.rotation_rad);
  const float sin_r = std::sin(r.rotation_rad);

  // Half-extent of the rotated rect's axis-aligned bounding box, in
  // aspect-corrected units: for a rectangle with local half-extents
  // (hw, hh) rotated by theta, the AABB half-extents are
  // |hw*cos| + |hh*sin| (x) and |hw*sin| + |hh*cos| (y).
  const float bound_half_w_px =
      std::fabs(half_w_px * cos_r) + std::fabs(half_h_px * sin_r);
  const float bound_half_h_px =
      std::fabs(half_w_px * sin_r) + std::fabs(half_h_px * cos_r);

  // Convert back to normalized space: x extents divide by aspect, y extents
  // stay in height units.
  const float bound_half_w = bound_half_w_px / aspect;
  const float bound_half_h = bound_half_h_px;

  return Rect{r.center.x - bound_half_w, r.center.y - bound_half_h,
             2.0f * bound_half_w, 2.0f * bound_half_h};
}

int MatchSlotForDetection(const std::vector<TrackSlot>& slots,
                          const PalmDetection& d, float iou_threshold) {
  int best_idx = -1;
  float best_iou = iou_threshold;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const TrackSlot& slot = slots[i];
    if (!slot.active) {
      continue;
    }
    // AxisAlignedBounds needs an aspect; since both slot.roi and d.box are
    // already in frame-normalized space by this file's contract, and IoU is
    // invariant under a per-axis scaling applied to both boxes, aspect 1.0
    // is a safe common space for this comparison (d.box is axis-aligned
    // already, so it carries no aspect dependence of its own).
    const Rect bounds = AxisAlignedBounds(slot.roi, 1.0f);
    const float iou = IoU(bounds, d.box);
    if (iou > best_iou) {
      best_iou = iou;
      best_idx = static_cast<int>(i);
    }
  }
  return best_idx;
}

std::vector<int> AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                                         const std::vector<PalmDetection>& dets,
                                         float iou_threshold) {
  std::vector<int> result(dets.size(), -1);

  struct Pair {
    std::size_t slot_index;
    std::size_t det_index;
    float iou;
  };

  std::vector<Pair> pairs;
  pairs.reserve(slots.size() * dets.size());
  for (std::size_t s = 0; s < slots.size(); ++s) {
    if (!slots[s].active) {
      continue;
    }
    const Rect bounds = AxisAlignedBounds(slots[s].roi, 1.0f);
    for (std::size_t j = 0; j < dets.size(); ++j) {
      const float iou = IoU(bounds, dets[j].box);
      if (iou > iou_threshold) {
        pairs.push_back(Pair{s, j, iou});
      }
    }
  }

  // Greedy by descending IoU. Ties broken deterministically by
  // (slot index, detection index) -- ruling T9-G -- so the result never
  // depends on input order.
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
    if (a.iou != b.iou) {
      return a.iou > b.iou;
    }
    if (a.slot_index != b.slot_index) {
      return a.slot_index < b.slot_index;
    }
    return a.det_index < b.det_index;
  });

  std::vector<bool> slot_consumed(slots.size(), false);
  std::vector<bool> det_consumed(dets.size(), false);

  for (const Pair& p : pairs) {
    if (slot_consumed[p.slot_index] || det_consumed[p.det_index]) {
      continue;
    }
    result[p.det_index] = static_cast<int>(p.slot_index);
    slot_consumed[p.slot_index] = true;
    det_consumed[p.det_index] = true;
  }

  return result;
}

std::uint32_t NextTrackId() {
  static std::atomic<std::uint32_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace hc
