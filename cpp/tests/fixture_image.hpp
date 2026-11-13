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
#include <cstring>
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

// Returns a horizontally mirrored copy of `src` (column x becomes column
// width-1-x). Used by the two-hand tests to manufacture a second hand that is
// the opposite handedness of the first: MediaPipe's handedness output is
// defined against the input image's own orientation, so mirroring the pixels
// must flip the reported side. See cpp/docs/models.md, "Handedness label
// order".
inline OwnedImage MirrorX(const OwnedImage& src) {
  OwnedImage out;
  const int w = src.view.width;
  const int h = src.view.height;
  out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                    3u);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* s =
        src.view.data + static_cast<std::size_t>(y) * src.view.stride;
    std::uint8_t* d =
        out.pixels.data() + static_cast<std::size_t>(y) * w * 3u;
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* sp = s + static_cast<std::size_t>(w - 1 - x) * 3u;
      std::uint8_t* dp = d + static_cast<std::size_t>(x) * 3u;
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
    }
  }
  out.view.data = out.pixels.data();
  out.view.width = w;
  out.view.height = h;
  out.view.stride = w * 3;
  out.view.format = src.view.format;
  return out;
}

// Composites `a` and `b` into one image of width a.w + b.w, with `a` on the
// left. Both inputs must have the same height and the same pixel format.
//
// WARNING (ruling T10-A): this makes the frame WIDER, and the palm detector
// letterboxes the whole frame into a square, so a side-by-side pair of 4:3
// fixtures is letterboxed into a square whose side is the COMBINED width.
// Every hand in it therefore lands roughly half as large in the detector's
// 192x192 input as the same hand does in a single-fixture frame, which pushes
// it toward the small end of the SSD anchor grid. Do not compensate by
// lowering the detector's min_score -- that is a model parameter pinned in
// cpp/docs/models.md -- and do not special-case the tracker. Compensate with
// the composite's own geometry, which is the test's free choice.
//
// MEASURED, so this stays a caveat rather than a blocker (cpp/docs/models.md,
// "Palm scores on multi-hand composites"): on the committed fixture the
// shrink is survivable and no compensation was needed. Sigmoid scores are
// 0.9364 for a lone hand, 0.8932 / 0.8799 for a side-by-side pair, and
// 0.8784 / 0.8784 / 0.8185 for three -- all clear the 0.5 gate with margin.
inline OwnedImage SideBySide(const OwnedImage& a, const OwnedImage& b) {
  if (a.view.height != b.view.height) {
    throw std::runtime_error("SideBySide: inputs must have equal height");
  }
  if (a.view.format != b.view.format) {
    throw std::runtime_error("SideBySide: inputs must have the same format");
  }
  const int h = a.view.height;
  const int w = a.view.width + b.view.width;

  OwnedImage out;
  out.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                        3u,
                    0);
  const int stride = w * 3;
  for (int y = 0; y < h; ++y) {
    std::uint8_t* d =
        out.pixels.data() + static_cast<std::size_t>(y) * stride;
    std::memcpy(d, a.view.data + static_cast<std::size_t>(y) * a.view.stride,
                static_cast<std::size_t>(a.view.width) * 3u);
    std::memcpy(d + static_cast<std::size_t>(a.view.width) * 3u,
                b.view.data + static_cast<std::size_t>(y) * b.view.stride,
                static_cast<std::size_t>(b.view.width) * 3u);
  }
  out.view.data = out.pixels.data();
  out.view.width = w;
  out.view.height = h;
  out.view.stride = stride;
  out.view.format = a.view.format;
  return out;
}

// A black image the same size and format as `like`. Used to blank one half of
// a composite so a hand disappears without changing the frame's geometry --
// changing the geometry would change the letterbox, and so would change the
// surviving hand's scale in the detector input too.
inline OwnedImage BlackLike(const OwnedImage& like) {
  return SolidImage(like.view.width, like.view.height, 0);
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
