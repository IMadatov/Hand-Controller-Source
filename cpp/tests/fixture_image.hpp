#pragma once

// Test-only image fixture helpers, shared by Task 8's single-hand tracker
// tests and Task 10's two-hand tests.
//
// Deliberately header-only and dependency-free (fread + <fstream> for the
// sidecar only): the core may not link an image library, and neither may its
// tests. The committed fixture is a raw interleaved RGB8 dump plus a
// "<w> <h> <c>" sidecar, exactly the pair Task 1 produced.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hc/hand_tracker.hpp"
#include "hc/image_view.hpp"

namespace hc_test {

// Pixel storage plus a view onto it. The view's `data` points into `pixels`,
// so OwnedImage must not be copied-then-destroyed while the view is in use;
// hold it by value in the test and use `.view`.
struct OwnedImage {
  std::vector<std::uint8_t> pixels;
  hc::ImageView view;
};

// Reads `<HC_FIXTURE_DIR>/<path_stem>.raw` + `.txt`. The sidecar carries
// "<width> <height> <channels>"; channels must be 3.
inline OwnedImage LoadRawFixture(const char* path_stem) {
  const std::string base = std::string(HC_FIXTURE_DIR) + "/" + path_stem;
  const std::string txt_path = base + ".txt";
  const std::string raw_path = base + ".raw";

  std::ifstream txt(txt_path);
  if (!txt) throw std::runtime_error("cannot open " + txt_path);
  int width = 0, height = 0, channels = 0;
  if (!(txt >> width >> height >> channels)) {
    throw std::runtime_error("malformed sidecar (expected '<w> <h> <c>'): " +
                             txt_path);
  }
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("non-positive dimensions in " + txt_path);
  }
  if (channels != 3) {
    throw std::runtime_error("fixture must have 3 channels, got " +
                             std::to_string(channels) + " in " + txt_path);
  }

  OwnedImage img;
  const std::size_t expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
  img.pixels.resize(expected);

  std::FILE* f = std::fopen(raw_path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + raw_path);
  const std::size_t got = std::fread(img.pixels.data(), 1, expected, f);
  std::fclose(f);
  if (got != expected) {
    throw std::runtime_error("short read on " + raw_path + ": expected " +
                             std::to_string(expected) + " bytes, got " +
                             std::to_string(got));
  }

  img.view.data = img.pixels.data();
  img.view.width = width;
  img.view.height = height;
  img.view.stride = width * 3;
  img.view.format = hc::PixelFormat::Rgb8;
  return img;
}

// A uniform `gray` image of the given size. Used for the "no hand in frame"
// cases: models.md records sigmoid(best palm score) = 0.057 on uniform
// mid-grey, well under the 0.5 gate.
inline OwnedImage SolidImage(int w, int h, std::uint8_t gray) {
  OwnedImage img;
  img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u,
                    gray);
  img.view.data = img.pixels.data();
  img.view.width = w;
  img.view.height = h;
  img.view.stride = w * 3;
  img.view.format = hc::PixelFormat::Rgb8;
  return img;
}

// Model paths come from compile definitions CMake fills in from
// HC_PALM_MODEL / HC_LANDMARK_MODEL, so tests never hardcode a build path.
inline hc::HandTrackerConfig DefaultTestConfig(int max_hands) {
  hc::HandTrackerConfig cfg;
  cfg.palm_model_path = HC_PALM_MODEL_PATH;
  cfg.landmark_model_path = HC_LANDMARK_MODEL_PATH;
  cfg.max_hands = max_hands;
  return cfg;
}

}  // namespace hc_test
