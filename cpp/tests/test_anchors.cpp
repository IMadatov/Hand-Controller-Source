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

  // Grid alignment and the stride-8-before-stride-16 macro order above hold
  // identically under a layer-major emission (three separate passes over
  // three 12x12 maps, 2 anchors per cell each): same 864 anchors, same
  // alignment, same macro order -- only the order *within* the stride-16
  // block differs. Task 6 decodes output rows positionally against this
  // vector, so the within-block order is load-bearing and must be pinned
  // directly. Assert cell-major adjacency: all anchors of one cell are
  // contiguous before the next cell begins.
  //
  // Stride 8: 2 anchors per cell (1 aspect ratio + 1 interpolated scale), so
  // anchors 0 and 1 share a center and anchor 2 advances to the next cell.
  EXPECT_NEAR(anchors[1].x_center, anchors[0].x_center, 1e-6f);
  EXPECT_NEAR(anchors[1].y_center, anchors[0].y_center, 1e-6f);
  EXPECT_NEAR(anchors[2].x_center, anchors[0].x_center + kStride8CellSize,
              1e-6f);
  EXPECT_NEAR(anchors[2].y_center, anchors[0].y_center, 1e-6f);

  // Stride 16: 6 anchors per cell (3 coalesced layers x 2). The first 6
  // anchors of the block must all share one center, and the 7th must differ
  // -- it is the next cell. Under layer-major, anchors 0 and 1 of the block
  // share a center but anchor 2 does not, so this discriminates.
  const Anchor& s16 = anchors[kStride8Count];
  for (std::size_t k = 1; k < 6; ++k) {
    const Anchor& a = anchors[kStride8Count + k];
    EXPECT_NEAR(a.x_center, s16.x_center, 1e-6f)
        << "stride-16 anchor " << k
        << " must share cell 0's center (cell-major emission)";
    EXPECT_NEAR(a.y_center, s16.y_center, 1e-6f)
        << "stride-16 anchor " << k
        << " must share cell 0's center (cell-major emission)";
  }
  const Anchor& s16_next_cell = anchors[kStride8Count + 6];
  EXPECT_NEAR(s16_next_cell.x_center, s16.x_center + kStride16CellSize, 1e-6f)
      << "the 7th stride-16 anchor must advance to the next cell";
  EXPECT_NEAR(s16_next_cell.y_center, s16.y_center, 1e-6f);
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
  constexpr float kStride16CellSize = 1.0f / 12.0f;
  const std::size_t stride16_count = anchors.size() - kStride8Count;
  EXPECT_EQ(stride16_count, std::size_t{144 * 6});
  EXPECT_EQ(stride16_count, std::size_t{864});

  // The bare total above is satisfied by a layer-major emission too, so
  // assert per-cell multiplicity *contiguously*: walk the stride-16 block in
  // chunks of 6 and require every chunk to be one cell repeated 6 times
  // (3 coalesced layers x 1 aspect ratio x 2 from the interpolated scale).
  // Under layer-major each center still appears 6 times overall, but chunk 0
  // would read cell0, cell0, cell1, cell1, cell2, cell2 -- so the
  // contiguity is what discriminates.
  constexpr std::size_t kAnchorsPerStride16Cell = 6;
  ASSERT_EQ(stride16_count % kAnchorsPerStride16Cell, std::size_t{0});
  const std::size_t num_cells = stride16_count / kAnchorsPerStride16Cell;
  EXPECT_EQ(num_cells, std::size_t{144});

  for (std::size_t cell = 0; cell < num_cells; ++cell) {
    const std::size_t base = kStride8Count + cell * kAnchorsPerStride16Cell;
    const Anchor& first = anchors[base];
    for (std::size_t k = 1; k < kAnchorsPerStride16Cell; ++k) {
      EXPECT_NEAR(anchors[base + k].x_center, first.x_center, 1e-6f)
          << "stride-16 cell " << cell << " anchor " << k
          << ": each cell's 6 anchors must be contiguous";
      EXPECT_NEAR(anchors[base + k].y_center, first.y_center, 1e-6f)
          << "stride-16 cell " << cell << " anchor " << k
          << ": each cell's 6 anchors must be contiguous";
    }
    // ...and consecutive chunks must be *different* cells, so the 144 chunks
    // cover 144 distinct centers rather than repeating one.
    if (cell + 1 < num_cells) {
      const Anchor& next = anchors[base + kAnchorsPerStride16Cell];
      const bool advanced =
          std::abs(next.x_center - first.x_center) > 0.5f * kStride16CellSize ||
          std::abs(next.y_center - first.y_center) > 0.5f * kStride16CellSize;
      EXPECT_TRUE(advanced)
          << "stride-16 chunk " << cell + 1
          << " must be a different cell than chunk " << cell;
    }
  }
}
