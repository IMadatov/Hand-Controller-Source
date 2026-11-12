#pragma once

#include <vector>

namespace hc {

// Parameters of MediaPipe's SsdAnchorsCalculator, mirrored field-for-field
// (see cpp/docs/models.md's "SSD anchor parameters" section, which pins
// every value from palm_detection_cpu.pbtxt plus the one field --
// interpolated_scale_aspect_ratio -- that the pbtxt omits because it relies
// on the proto2 default).
struct AnchorOptions {
  int input_w, input_h;
  int num_layers;
  float min_scale, max_scale;
  float anchor_offset_x, anchor_offset_y;
  std::vector<int> strides;
  bool fixed_anchor_size;
  std::vector<float> aspect_ratios;
  float interpolated_scale_aspect_ratio;
};

// A single SSD anchor box, normalized to the input the detector ran on.
// With fixed_anchor_size (as the palm model uses), w and h are always 1.0.
struct Anchor {
  float x_center, y_center, w, h;
};

// Generates the SSD anchor grid, mirroring MediaPipe's
// SsdAnchorsCalculator::GenerateAnchors: layers are walked in order, runs of
// consecutive layers sharing the same stride are coalesced into a single
// feature-map pass (accumulating each layer's aspect ratios and scales), and
// anchors are emitted row-major within each pass. Anchors from finer
// strides precede anchors from coarser strides, matching the palm model's
// output row order.
std::vector<Anchor> GenerateAnchors(const AnchorOptions& o);

// The SSD anchor parameters for MediaPipe's palm detector, pinned verbatim
// from cpp/docs/models.md.
AnchorOptions PalmAnchorOptions();

}  // namespace hc
