#include "hc/anchors.hpp"

#include <cmath>
#include <cstddef>

namespace hc {

namespace {

// Mirrors MediaPipe's SsdAnchorsCalculator::CalculateScale: linearly
// interpolates between min_scale and max_scale across num_strides layers.
// stride_index is 0-based and may run from 0 to num_strides inclusive (the
// one-past-the-end call is used to get the "next" scale for the
// interpolated aspect ratio).
float CalculateScale(float min_scale, float max_scale, int stride_index,
                      int num_strides) {
  if (num_strides == 1) {
    return (min_scale + max_scale) * 0.5f;
  }
  return min_scale +
         (max_scale - min_scale) * static_cast<float>(stride_index) /
             static_cast<float>(num_strides - 1);
}

}  // namespace

std::vector<Anchor> GenerateAnchors(const AnchorOptions& o) {
  std::vector<Anchor> anchors;

  const int num_strides = static_cast<int>(o.strides.size());
  std::size_t layer_id = 0;
  while (layer_id < o.strides.size()) {
    std::vector<float> anchor_height;
    std::vector<float> anchor_width;
    std::vector<float> aspect_ratios;
    std::vector<float> scales;

    // Coalesce every consecutive layer that shares this stride, matching
    // MediaPipe's inner "while (last_same_stride_layer < strides.size() &&
    // strides[last_same_stride_layer] == stride)" loop. Each coalesced
    // layer contributes its own full set of aspect_ratios plus (if
    // interpolated_scale_aspect_ratio > 0) one extra interpolated anchor.
    std::size_t last_same_stride_layer = layer_id;
    while (last_same_stride_layer < o.strides.size() &&
           o.strides[last_same_stride_layer] == o.strides[layer_id]) {
      const float scale = CalculateScale(
          o.min_scale, o.max_scale, static_cast<int>(last_same_stride_layer),
          num_strides);
      for (float aspect_ratio : o.aspect_ratios) {
        aspect_ratios.push_back(aspect_ratio);
        scales.push_back(scale);
      }
      if (o.interpolated_scale_aspect_ratio > 0.0f) {
        const float scale_next =
            (last_same_stride_layer == o.strides.size() - 1)
                ? 1.0f
                : CalculateScale(o.min_scale, o.max_scale,
                                  static_cast<int>(last_same_stride_layer) + 1,
                                  num_strides);
        scales.push_back(std::sqrt(scale * scale_next));
        aspect_ratios.push_back(o.interpolated_scale_aspect_ratio);
      }
      ++last_same_stride_layer;
    }

    for (std::size_t i = 0; i < aspect_ratios.size(); ++i) {
      const float ratio_sqrt = std::sqrt(aspect_ratios[i]);
      anchor_height.push_back(scales[i] / ratio_sqrt);
      anchor_width.push_back(scales[i] * ratio_sqrt);
    }

    const int stride = o.strides[layer_id];
    const int feature_map_height =
        static_cast<int>(std::ceil(static_cast<float>(o.input_h) / stride));
    const int feature_map_width =
        static_cast<int>(std::ceil(static_cast<float>(o.input_w) / stride));

    for (int y = 0; y < feature_map_height; ++y) {
      for (int x = 0; x < feature_map_width; ++x) {
        for (std::size_t anchor_id = 0; anchor_id < anchor_height.size();
             ++anchor_id) {
          const float x_center =
              (static_cast<float>(x) + o.anchor_offset_x) /
              static_cast<float>(feature_map_width);
          const float y_center =
              (static_cast<float>(y) + o.anchor_offset_y) /
              static_cast<float>(feature_map_height);

          Anchor anchor;
          anchor.x_center = x_center;
          anchor.y_center = y_center;
          if (o.fixed_anchor_size) {
            anchor.w = 1.0f;
            anchor.h = 1.0f;
          } else {
            anchor.w = anchor_width[anchor_id];
            anchor.h = anchor_height[anchor_id];
          }
          anchors.push_back(anchor);
        }
      }
    }

    layer_id = last_same_stride_layer;
  }

  return anchors;
}

AnchorOptions PalmAnchorOptions() {
  AnchorOptions o;
  o.input_w = 192;
  o.input_h = 192;
  o.num_layers = 4;
  o.min_scale = 0.1484375f;
  o.max_scale = 0.75f;
  o.anchor_offset_x = 0.5f;
  o.anchor_offset_y = 0.5f;
  o.strides = {8, 16, 16, 16};
  o.fixed_anchor_size = true;
  o.aspect_ratios = {1.0f};
  o.interpolated_scale_aspect_ratio = 1.0f;
  return o;
}

}  // namespace hc
