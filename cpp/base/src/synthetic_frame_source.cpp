#include "hc/synthetic_frame_source.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>

namespace hc {
namespace {

std::int64_t SteadyNowUs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

}  // namespace

SyntheticFrameSource::SyntheticFrameSource(int width, int height, int frame_count)
    : width_(width), height_(height), frame_count_(frame_count) {
  assert(width > 0 && height > 0);
  ended_ = (frame_count_ == 0);
  buffer_.assign(static_cast<std::size_t>(width_) * height_ * 3, 0);
}

bool SyntheticFrameSource::Next(Frame& out) {
  if (ended_) return false;
  if (frame_count_ > 0 && next_index_ >= static_cast<std::uint64_t>(frame_count_)) {
    ended_ = true;
    return false;
  }

  // A diagonal gradient shifted by the frame index, so that no two
  // consecutive frames can be byte-identical (the shift is +1 per frame,
  // and the pattern's period in the shift is 256 frames).
  const auto phase = static_cast<std::uint32_t>(next_index_ & 0xFFu);
  for (int y = 0; y < height_; ++y) {
    std::uint8_t* row = buffer_.data() + static_cast<std::size_t>(y) * width_ * 3;
    for (int x = 0; x < width_; ++x) {
      const std::uint32_t base = static_cast<std::uint32_t>(x + y) + phase;
      row[x * 3 + 0] = static_cast<std::uint8_t>(base & 0xFFu);
      row[x * 3 + 1] = static_cast<std::uint8_t>((base * 3u + 17u) & 0xFFu);
      row[x * 3 + 2] = static_cast<std::uint8_t>((base * 7u + 83u) & 0xFFu);
    }
  }

  out.view.data = buffer_.data();
  out.view.width = width_;
  out.view.height = height_;
  out.view.stride = width_ * 3;
  out.view.format = PixelFormat::Rgb8;
  out.timestamp_us = SteadyNowUs();
  out.index = next_index_;
  ++next_index_;
  return true;
}

void SyntheticFrameSource::Close() { ended_ = true; }

}  // namespace hc
