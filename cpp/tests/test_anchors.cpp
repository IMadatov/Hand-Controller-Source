#include "hc/anchors.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

using hc::Anchor;
using hc::AnchorOptions;
using hc::GenerateAnchors;
using hc::PalmAnchorOptions;

namespace {
// Recorded in cpp/docs/models.md (lines 160-175), observed three independent
// ways: ONNX output tensor shapes [1, 2016, 18] / [1, 2016, 1], the literal
// num_boxes: 2016 field in palm_detection_cpu.pbtxt, and the generation
// arithmetic itself (576*2 + 144*3*2 = 2016).
constexpr std::size_t kAnchorCountFromModelsMd = 2016;
}  // namespace

TEST(Anchors, PalmAnchorCountMatchesModelOutputRows) {
  // The count recorded in models.md, which must equal the palm model's
  // regressor output row count.
  EXPECT_EQ(GenerateAnchors(PalmAnchorOptions()).size(), kAnchorCountFromModelsMd);
}

TEST(Anchors, FirstAnchorIsCenteredInFirstCell) {
  // stride 8 over a 192x192 input: feature map is 24x24, so anchor[0]'s
  // center is (0 + anchor_offset) / 24 on both axes. With fixed_anchor_size,
  // w == h == 1.0 regardless of scale.
  const std::vector<Anchor> anchors = GenerateAnchors(PalmAnchorOptions());
  ASSERT_FALSE(anchors.empty());
  const Anchor& a0 = anchors.front();
  EXPECT_NEAR(a0.x_center, 0.5f / 24.0f, 1e-6f);
  EXPECT_NEAR(a0.y_center, 0.5f / 24.0f, 1e-6f);
  EXPECT_FLOAT_EQ(a0.w, 1.0f);
  EXPECT_FLOAT_EQ(a0.h, 1.0f);
}

TEST(Anchors, AnchorsAreGroupedByStrideInOrder) {
  // All anchors from the finest stride (8, feature map 24x24, 2 anchors per
  // cell => 1152 anchors) must precede those from the coarser stride (16,
  // feature map 12x12, 3 coalesced layers x 2 anchors per cell => 864
  // anchors), matching the model's output row order.
  const std::vector<Anchor> anchors = GenerateAnchors(PalmAnchorOptions());
  ASSERT_EQ(anchors.size(), std::size_t{2016});

  constexpr std::size_t kStride8Count = 576 * 2;  // 1152
  constexpr float kStride8CellSize = 1.0f / 24.0f;
  constexpr float kStride16CellSize = 1.0f / 12.0f;

  // Every anchor in the first 1152 belongs to the finer (stride-8) grid:
  // its center must be reachable as (i + 0.5) / 24 for some integer i in
  // [0, 24), which bounds every coordinate to within half a stride-8 cell
  // of a stride-8 grid point and strictly finer than the stride-16 grid.
  for (std::size_t i = 0; i < kStride8Count; ++i) {
    const Anchor& a = anchors[i];
    // Coordinate must land on a multiple-of-(1/24) grid (offset 0.5).
    const float scaled_x = a.x_center / kStride8CellSize - 0.5f;
    const float scaled_y = a.y_center / kStride8CellSize - 0.5f;
    EXPECT_NEAR(scaled_x, std::round(scaled_x), 1e-4f);
    EXPECT_NEAR(scaled_y, std::round(scaled_y), 1e-4f);
  }

  // The remaining 864 anchors belong to the coarser (stride-16) grid: their
  // centers land on the coarser (1/12) grid, not the finer (1/24) grid
  // extra points (i.e. they are not simply a continuation of the stride-8
  // sequence).
  for (std::size_t i = kStride8Count; i < anchors.size(); ++i) {
    const Anchor& a = anchors[i];
    const float scaled_x = a.x_center / kStride16CellSize - 0.5f;
    const float scaled_y = a.y_center / kStride16CellSize - 0.5f;
    EXPECT_NEAR(scaled_x, std::round(scaled_x), 1e-4f);
    EXPECT_NEAR(scaled_y, std::round(scaled_y), 1e-4f);
  }
}

TEST(Anchors, DuplicateStridesProduceDistinctAnchorsPerLayer) {
  // The palm options repeat stride 16 across three layers. Those layers are
  // coalesced by the same-stride inner loop into a single 12x12 pass, but
  // still contribute 3 layers x 2 anchors (aspect_ratios + interpolated
  // scale) = 6 anchors per cell, not 1 layer's worth (2 anchors per cell).
  // Verify by checking the total anchor count attributable to stride 16:
  // 144 cells x 6 anchors per cell == 864, which combined with the 1152 from
  // stride 8 is exactly 2016 -- if the duplicate strides collapsed into one
  // layer's worth of anchors, this total would instead be 576*2 + 144*2 =
  // 1440.
  const std::vector<Anchor> anchors = GenerateAnchors(PalmAnchorOptions());
  constexpr std::size_t kStride8Count = 576 * 2;
  const std::size_t stride16_count = anchors.size() - kStride8Count;
  EXPECT_EQ(stride16_count, std::size_t{144 * 6});
  EXPECT_EQ(stride16_count, std::size_t{864});
}
