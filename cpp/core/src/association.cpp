#include "hc/association.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
                          const PalmDetection& d, float aspect,
                          float iou_threshold) {
  int best_idx = -1;
  float best_iou = iou_threshold;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const TrackSlot& slot = slots[i];
    if (!slot.active) {
      continue;
    }
    // The real frame aspect, not a convenient constant: slot.roi is rotated,
    // and the hull of a rotated rect is aspect-dependent in shape, not just
    // in scale. See the header for why no IoU invariance rescues a hardcode.
    const Rect bounds = AxisAlignedBounds(slot.roi, aspect);
    const float iou = IoU(bounds, d.box);
    // Strictly greater, so on an exact tie the lower slot index wins --
    // the same deterministic (slot index, detection index) ordering
    // AssignDetectionsToSlots uses.
    if (iou > best_iou) {
      best_iou = iou;
      best_idx = static_cast<int>(i);
    }
  }
  return best_idx;
}

void AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                             const std::vector<PalmDetection>& dets,
                             float aspect, float iou_threshold,
                             std::vector<int>& out) {
  // The result vector is now the CALLER's buffer, so a per-frame caller
  // that reuses it allocates nothing at all here: assign() keeps existing
  // capacity. Everything else below is fixed-capacity too; the shape this
  // replaced (a pairs vector plus two std::vector<bool>s) put three heap
  // allocations on this path.
  std::vector<int>& result = out;
  result.assign(dets.size(), -1);

  const std::size_t n_slots = std::min(slots.size(), kMaxAssociationEntries);
  const std::size_t n_dets = std::min(dets.size(), kMaxAssociationEntries);

  // Slot hulls depend only on the slot, not on the detection, so they are
  // computed once here rather than once per (slot, detection) pair.
  std::array<Rect, kMaxAssociationEntries> slot_bounds{};
  for (std::size_t s = 0; s < n_slots; ++s) {
    if (!slots[s].active) {
      continue;
    }
    slot_bounds[s] = AxisAlignedBounds(slots[s].roi, aspect);
  }

  // Consumed-side bookkeeping as bitmasks instead of two heap vectors.
  std::uint64_t slot_consumed = 0;
  std::uint64_t det_consumed = 0;

  // Greedy by descending IoU, expressed as "repeatedly take the best
  // remaining pair" rather than "build every pair, sort, then walk". The
  // two are equivalent -- each iteration picks the globally largest IoU
  // among unconsumed pairs -- but this form needs no pair buffer and no
  // sort. At most min(n_slots, n_dets) pairs can ever be accepted, so that
  // bounds the outer loop.
  const std::size_t max_pairs = std::min(n_slots, n_dets);
  for (std::size_t accepted = 0; accepted < max_pairs; ++accepted) {
    std::size_t best_s = 0;
    std::size_t best_j = 0;
    float best_iou = iou_threshold;
    bool found = false;

    // Scanning slots ascending and detections ascending, and replacing the
    // incumbent only on a STRICTLY greater IoU, makes ties resolve to the
    // lowest (slot index, detection index) -- ruling T9-G -- so the result
    // never depends on input order.
    for (std::size_t s = 0; s < n_slots; ++s) {
      if (!slots[s].active || (slot_consumed & (std::uint64_t{1} << s))) {
        continue;
      }
      for (std::size_t j = 0; j < n_dets; ++j) {
        if (det_consumed & (std::uint64_t{1} << j)) {
          continue;
        }
        const float iou = IoU(slot_bounds[s], dets[j].box);
        if (iou > best_iou) {
          best_iou = iou;
          best_s = s;
          best_j = j;
          found = true;
        }
      }
    }

    if (!found) {
      break;
    }

    result[best_j] = static_cast<int>(best_s);
    slot_consumed |= std::uint64_t{1} << best_s;
    det_consumed |= std::uint64_t{1} << best_j;
  }

}

// Thin wrapper: the by-value form every non-per-frame caller (and every
// test) uses. It allocates exactly the one vector it returns.
std::vector<int> AssignDetectionsToSlots(const std::vector<TrackSlot>& slots,
                                         const std::vector<PalmDetection>& dets,
                                         float aspect, float iou_threshold) {
  std::vector<int> out;
  AssignDetectionsToSlots(slots, dets, aspect, iou_threshold, out);
  return out;
}

std::uint32_t NextTrackId() {
  static std::atomic<std::uint32_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace hc
