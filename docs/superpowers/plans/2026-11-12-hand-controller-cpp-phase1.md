# Hand Controller C++ — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `FrameSource` → `HandTracker` in C++ so that two hands are tracked from a frame stream with 21 normalized landmarks each, and produce a per-stage timing report that lets the owner decide whether the ONNX approach is viable.

**Architecture:** A `hc_core` static library holds the portable layers and links neither OpenCV nor X11 — enforced at configure time. Hand tracking mirrors MediaPipe's own two-model structure (palm detector run only for untracked hands, landmark model run per tracked hand on a rotated crop), with the SSD anchor decoding, NMS, ROI derivation and the rotated-crop resampler all written by hand because they live inside MediaPipe's graph and do not ship with the model files. The `hand_controller` executable holds the OpenCV-backed camera/file frame source and the debug overlay.

**Tech Stack:** C++17, CMake 3.28 + Ninja, clang 18, ONNX Runtime (prebuilt release archive fetched by CMake), OpenCV (built from source by CMake, trimmed module list, app target only), GoogleTest (fetched by CMake).

**Spec:** `docs/superpowers/specs/2026-11-12-hand-controller-cpp-design.md`

## Global Constraints

Every task's requirements implicitly include this section.

- **C++17.** Chosen for Android NDK compatibility; do not use C++20 features.
- **The core speaks normalized `0.0..1.0` coordinates, never pixels.** Conversion to pixels happens only in `Overlay`.
- **`hc_core` must link neither OpenCV nor X11.** OpenCV is permitted only in the app's frame source and overlay. No OpenCV type may appear in any core signature.
- **The core sees `ImageView { data, width, height, stride, format }`** — a non-owning view over pixels.
- **A clean `git clone` plus `cmake` must be enough.** No system packages, no `vcpkg`, no `conan`, no Bazel.
- **ONNX Runtime comes from the prebuilt official release archive** fetched by CMake. Do not build it from source (pulls in Python and protobuf).
- **OpenCV is built from source with its module list trimmed** to only what is used.
- **Model files are fetched by CMake at configure time with pinned checksums** and are not committed.
- **Two hands are tracked simultaneously, with fully independent per-hand state.**
- **Everything must be developable and testable with no camera attached.** There is no `/dev/video*` on the development machine.
- **Phase 1 ends with the measurement report.** Do not begin phase 2. Do not silently start optimizing if the numbers are poor — report them and stop.
- **Available:** cmake 3.28.3, clang++ 18.1.3, ninja 1.11.1. **Absent:** `g++`, `bazel`, `vcpkg`, `conan`, system OpenCV, system ONNX Runtime.
- **Out of phase 1 scope:** draw mode, gesture recognition, action mapping, every `OutputSink`.
- **Do not modify `main.py`.** The Python version stays as it is.
- **Phase 1 contains no Windows-specific source code.** The spec's "Windows backends written but unverified" refers to `OutputSink` (`SendInput`), which is phase 2. In phase 1 the only platform-touching code is the frame source, and OpenCV's `videoio` reaches DirectShow through the same API it uses for V4L2 — so there is nothing to branch on. What is genuinely unverified here is the Windows *build*: the ONNX Runtime `win-x64` archive and the Windows OpenCV source build have not been run. Do not claim otherwise in `cpp/README.md`.

## Review Focus

Five conditions the spec implies that a reasonable user will hit, each pinned to a test in the task that owns the code:

1. **A hand partially outside the frame** gives an ROI extending past the image bounds. The resampler must clamp or zero rather than read out of bounds — currently the most likely source of a crash or of garbage landmarks. *(Task 4)*
2. **No hand in the frame at all** must yield zero observations and no error. This is spec defect 2: the Python version raised `UnboundLocalError` on an empty hand. *(Task 8)*
3. **Two hands crossing or overlapping** must not swap identities, and the palm detector returning a hand that is already tracked must not create a third. The spec warns that the model's own handedness output flips per-crop and cannot be the primary signal. *(Task 9)*
4. **A frame source that fails mid-run** — which a phone camera over the network will do — must end cleanly without deadlocking the capture thread or spinning a busy loop. *(Task 11)*
5. **A frame whose stride exceeds `width × channels`**, which V4L2 and MJPEG decode routinely produce. Any resampler that assumes packed rows silently garbles the image. *(Task 4)*

---

### Task 1: Validate the ONNX models and pin their parameters

The spec names this the first task: if the models do not work, nothing else matters. The deliverable is recorded knowledge, not architecture. **Every expected value below is a hypothesis to confirm against the real files, not a given** — later tasks consume what this task records, so record what is true.

**Files:**
- Create: `cpp/CMakeLists.txt` (fetch + `tools` target only; Task 2 adds the rest)
- Create: `cpp/cmake/FetchOnnxRuntime.cmake`
- Create: `cpp/cmake/FetchModels.cmake`
- Create: `cpp/tools/validate_models.cpp`
- Create: `cpp/docs/models.md`
- Create: `cpp/tests/fixtures/hand_rgb.raw`, `cpp/tests/fixtures/hand_rgb.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `cpp/docs/models.md`, the single source of truth for Tasks 4–10. It must record, for each model: input tensor name, shape, dtype and value range; every output tensor name, shape and meaning; the SSD anchor parameters (`num_layers`, `min_scale`, `max_scale`, `strides`, `anchor_offset_x/y`, `fixed_anchor_size`, resulting anchor count); the palm score threshold, score clipping threshold and NMS IoU; and the ROI transform constants for both palm→ROI and landmarks→ROI (`scale_x`, `scale_y`, `shift_x`, `shift_y`, `square_long`, and which two keypoint/landmark indices define the rotation axis). Also produces CMake cache variables `HC_PALM_MODEL` and `HC_LANDMARK_MODEL` holding absolute paths to the fetched models.

- [ ] **Step 1: Obtain a fixture image containing one clearly visible hand**

Commit it as raw interleaved RGB8 at `cpp/tests/fixtures/hand_rgb.raw`, with `cpp/tests/fixtures/hand_rgb.txt` containing `<width> <height> 3` on one line. Raw rather than PNG so that core tests can read it with `fread` and no image library — core may not link OpenCV. If the owner has a frame from their DroidCam setup, that is the best source; otherwise any permissively-licensed hand photograph works.

- [ ] **Step 2: Set the language standard in `cpp/CMakeLists.txt`**

`cmake_minimum_required(VERSION 3.28)`, `set(CMAKE_CXX_STANDARD 17)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`, `set(CMAKE_CXX_EXTENSIONS OFF)`. The standard is a global constraint and must be pinned here, in the first file created, rather than inherited by accident.

- [ ] **Step 3: Write `cpp/cmake/FetchOnnxRuntime.cmake`**

Use `FetchContent_Declare` with a per-platform `URL` pointing at the official ONNX Runtime release archive (`onnxruntime-linux-x64-<ver>.tgz`, `onnxruntime-win-x64-<ver>.zip`), `URL_HASH SHA256=...`, and `DOWNLOAD_EXTRACT_TIMESTAMP TRUE`. Expose an imported target `onnxruntime::onnxruntime` with its include dir and library. Pin a specific version and record it in `models.md`.

- [ ] **Step 4: Write `cpp/cmake/FetchModels.cmake`**

Fetch pre-converted ONNX palm-detection and hand-landmark models by URL with `URL_HASH SHA256=...`. Set `HC_PALM_MODEL` and `HC_LANDMARK_MODEL` to the resulting absolute paths. Download each file once by hand first to compute its SHA256, and pin that.

- [ ] **Step 5: Write `cpp/tools/validate_models.cpp`**

A standalone program that, for each of the two models: creates an `Ort::Session`, then prints every input and output tensor's name, shape and element type. Then loads `hand_rgb.raw`, letterboxes it to the palm model's input size, runs the palm model, and prints the highest score found and the decoded raw box row at that index. Then runs the landmark model on the full image resized to its input size and prints all 21 raw landmark triples plus the presence and handedness outputs.

- [ ] **Step 6: Build and run it**

Run: `cmake -S cpp -B cpp/build -G Ninja -DCMAKE_CXX_COMPILER=clang++ && cmake --build cpp/build --target validate_models && ./cpp/build/tools/validate_models`
Expected: both sessions create without throwing; every tensor shape prints; the palm model's best score is high enough to be a real detection rather than noise; the 21 printed landmarks are not all identical or all zero.

- [ ] **Step 7: Write `cpp/docs/models.md`**

Record everything listed in this task's Interfaces block, with the observed values. Where an anchor or ROI constant cannot be read off the model file, state the source it was taken from (MediaPipe's own graph configuration) and mark it as needing confirmation by Task 7's end-to-end test.

- [ ] **Step 8: Stop and report if the models are unusable**

If scores are noise, outputs are missing, or landmarks are degenerate, do not proceed to Task 2. Report what was observed. The spec accepts that converting the models afresh is roughly a day of additional work, and that decision is the owner's.

- [ ] **Step 9: Commit**

```bash
git add cpp/CMakeLists.txt cpp/cmake cpp/tools cpp/docs/models.md cpp/tests/fixtures
git commit -m "feat: fetch and validate ONNX hand models, pin their parameters"
```

---

### Task 2: Project skeleton, core types, and layering enforcement

**Files:**
- Modify: `cpp/CMakeLists.txt`
- Create: `cpp/cmake/HcAssertNoForbiddenDeps.cmake`
- Create: `cpp/core/CMakeLists.txt`
- Create: `cpp/core/include/hc/image_view.hpp`
- Create: `cpp/core/include/hc/geometry.hpp`
- Create: `cpp/core/include/hc/hand_observation.hpp`
- Create: `cpp/core/src/geometry.cpp`
- Create: `cpp/tests/CMakeLists.txt`
- Test: `cpp/tests/test_geometry.cpp`

**Interfaces:**
- Consumes: nothing from Task 1 except that `cpp/CMakeLists.txt` now exists.
- Produces:
  - `enum class PixelFormat { Rgb8, Bgr8 };`
  - `struct ImageView { const std::uint8_t* data; int width; int height; int stride; PixelFormat format; };` — `stride` is bytes per row and may exceed `width * 3`.
  - `struct Point2 { float x, y; };` — normalized to the full frame.
  - `struct Rect { float x, y, w, h; };` — normalized, axis-aligned.
  - `struct RotatedRect { Point2 center; float w, h; float rotation_rad; };` — `w` normalized to frame width, `h` to frame height.
  - `Rect BoundingBox(const Point2* pts, std::size_t n);`
  - `float IoU(const Rect& a, const Rect& b);`
  - `RotatedRect SquareLong(const RotatedRect& r, float aspect);` — `aspect` is frame width / height; makes the rect square **in pixels**, taking the longer side.
  - `RotatedRect ExpandAndShift(const RotatedRect& r, float scale_x, float scale_y, float shift_x, float shift_y);` — shifts are fractions of the rect's own size, applied along the rect's rotated axes.
  - `enum class Handedness { Unknown, Left, Right };`
  - `struct HandObservation { std::uint32_t track_id; Handedness handedness; float handedness_score; float presence; std::array<Point2, 21> landmarks; std::int64_t timestamp_us; };`
  - CMake function `hc_assert_no_forbidden_deps(<target>)`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Geometry, BoundingBoxOfThreePoints) {
  const Point2 pts[] = {{0.2f, 0.5f}, {0.4f, 0.1f}, {0.3f, 0.9f}};
  const Rect b = BoundingBox(pts, 3);
  EXPECT_FLOAT_EQ(b.x, 0.2f); EXPECT_FLOAT_EQ(b.y, 0.1f);
  EXPECT_FLOAT_EQ(b.w, 0.2f); EXPECT_FLOAT_EQ(b.h, 0.8f);
}

TEST(Geometry, IoUOfIdenticalRectsIsOne) { /* IoU({0,0,.5,.5},{0,0,.5,.5}) == 1 */ }
TEST(Geometry, IoUOfDisjointRectsIsZero) { /* IoU({0,0,.2,.2},{.5,.5,.2,.2}) == 0 */ }
TEST(Geometry, IoUOfHalfOverlapIsOneThird) { /* {0,0,.2,.2} vs {.1,0,.2,.2} -> 1/3 */ }

TEST(Geometry, SquareLongIsSquareInPixelsNotNormalized) {
  // aspect 16/9: a rect 0.2 wide and 0.2 tall is 1.78x wider in pixels than tall,
  // so squaring on the long side must grow h, not w.
  const RotatedRect r = SquareLong({{0.5f, 0.5f}, 0.2f, 0.2f, 0.0f}, 16.0f / 9.0f);
  EXPECT_FLOAT_EQ(r.w, 0.2f);
  EXPECT_FLOAT_EQ(r.h, 0.2f * 16.0f / 9.0f);
}

TEST(Geometry, ExpandAndShiftScalesAboutCenterWhenShiftIsZero) { /* scale 2,2 shift 0,0 */ }
TEST(Geometry, ExpandAndShiftMovesAlongRotatedAxis) {
  // rotation = pi/2, shift_y = -0.5 must move the center along -x, not -y.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake -S cpp -B cpp/build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DHC_BUILD_APP=OFF && cmake --build cpp/build && ctest --test-dir cpp/build`
Expected: compile failure — `geometry.hpp` declares functions with no definitions.

- [ ] **Step 3: Implement the headers and `geometry.cpp`**

Signatures exactly as listed in Interfaces. `ExpandAndShift` applies shifts along the rect's rotated axes, which the rotation test pins.

- [ ] **Step 4: Add the `HC_BUILD_APP` option and the dependency assertion**

Add a CMake option `HC_BUILD_APP` defaulting to `ON`. When `OFF`, do not fetch or build OpenCV and do not add the app target. This matters for every later task: OpenCV from source is a slow first configure, and Tasks 2–10 do not need it, so their test cycles run with `-DHC_BUILD_APP=OFF`. Write `hc_assert_no_forbidden_deps(<target>)` to walk the target's `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` and `message(FATAL_ERROR ...)` on any entry matching `opencv`, `X11` or `Xtst`, case-insensitively. Call it on `hc_core`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure`
Expected: all PASS, and configure succeeds with the assertion active.

- [ ] **Step 6: Verify the assertion actually fires**

Temporarily add `target_link_libraries(hc_core PRIVATE X11)` and re-run configure.
Expected: `FATAL_ERROR` naming `X11`. Remove the line afterwards. A guard that cannot fail is not a guard.

- [ ] **Step 7: Commit**

```bash
git add cpp/CMakeLists.txt cpp/cmake cpp/core cpp/tests
git commit -m "feat: core geometry and image types with layering enforcement"
```

---

### Task 3: Per-stage metrics

Built before the tracker because Task 8's tests assert on the palm-detector call count, which this records.

**Files:**
- Create: `cpp/core/include/hc/metrics.hpp`
- Create: `cpp/core/src/metrics.cpp`
- Test: `cpp/tests/test_metrics.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class Stage { CaptureWait, Preprocess, PalmDetect, Landmark, Associate, Total, kCount };` — these are the stages the spec requires the report to break down.
  - `class Metrics` with `void Add(Stage s, double ms);`, `void Bump(Stage s);` (count without a duration), `struct StageStats { std::uint64_t calls; double mean_ms, p50_ms, p95_ms; };`, `StageStats Get(Stage s) const;`, `std::string Format() const;`, `void Reset();`
  - `class ScopedStage { public: ScopedStage(Metrics&, Stage); ~ScopedStage(); };` — times its own lifetime.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Metrics, PercentilesOverKnownSamples) {
  Metrics m;
  for (int i = 1; i <= 100; ++i) m.Add(Stage::Landmark, static_cast<double>(i));
  const auto s = m.Get(Stage::Landmark);
  EXPECT_EQ(s.calls, 100u);
  EXPECT_NEAR(s.mean_ms, 50.5, 1e-9);
  EXPECT_NEAR(s.p50_ms, 50.0, 1.0);
  EXPECT_NEAR(s.p95_ms, 95.0, 1.0);
}

TEST(Metrics, BumpCountsWithoutDuration) {
  Metrics m; m.Bump(Stage::PalmDetect); m.Bump(Stage::PalmDetect);
  EXPECT_EQ(m.Get(Stage::PalmDetect).calls, 2u);
}

TEST(Metrics, EmptyStageReportsZeroCalls) { /* Get on an untouched stage: calls == 0 */ }
TEST(Metrics, ResetClearsAllStages) { /* Add then Reset then calls == 0 */ }
TEST(Metrics, FormatNamesEveryStageWithSamples) {
  // Format() output contains "landmark" and the call count; a stage with no
  // samples must not appear as a misleading 0.00 ms row.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R Metrics --output-on-failure`
Expected: compile failure — `Metrics` undefined.

- [ ] **Step 3: Implement `Metrics` and `ScopedStage`**

Keep per-stage sample vectors; compute percentiles by sorting a copy on read, so `Add` stays cheap enough to call per frame.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R Metrics --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/metrics.hpp cpp/core/src/metrics.cpp cpp/tests/test_metrics.cpp
git commit -m "feat: per-stage timing metrics with percentiles"
```

---

### Task 4: Rotated-crop resampler

The single piece of image processing the core needs. Written by hand rather than with `cv::warpAffine` so that `hc_core` stays free of OpenCV, per the spec's anti-goal. It also replaces the Python version's full-frame colour conversion entirely: one warp reads straight from the source frame into the model's input tensor, in the right channel order, at the right size.

**Files:**
- Create: `cpp/core/include/hc/warp.hpp`
- Create: `cpp/core/src/warp.cpp`
- Test: `cpp/tests/test_warp.cpp`

**Interfaces:**
- Consumes: `ImageView`, `RotatedRect`, `Point2` from Task 2.
- Produces:
  - `struct WarpOptions { int side; float scale = 1.0f / 255.0f; float bias = 0.0f; bool output_rgb = true; };`
  - `void WarpToTensorNchw(const ImageView& src, const RotatedRect& roi, const WarpOptions& o, float* dst);` — writes `3 * o.side * o.side` floats, planar CHW, bilinear, sampling outside the source clamped to zero. `roi.w` is interpreted against `src.width` and `roi.h` against `src.height`.
  - `Point2 CropToFrame(const RotatedRect& roi, Point2 p_crop);` — maps a point in the crop's own `0..1` space back to frame-normalized coordinates. This is how landmark output returns to frame space.

- [ ] **Step 1: Write the failing tests**

```cpp
// Helper: a width x height RGB8 buffer where pixel (x,y) = {x%256, y%256, 0},
// with a caller-chosen stride.
TEST(Warp, IdentityRoiReproducesSource) {
  // 8x8 source, roi centered 0.5,0.5 w=1 h=1 rotation=0, side=8.
  // Every output pixel equals the source pixel, scaled.
}

TEST(Warp, HonorsStridePadding) {  // Review Focus 5
  // Same 8x8 image, once with stride = 24 and once with stride = 40.
  // Both warps must produce byte-identical tensors. A resampler that assumes
  // packed rows passes the first and garbles the second.
}

TEST(Warp, SamplingOutsideSourceIsZero) {  // Review Focus 1
  // roi centered at 0.95,0.95 with w=h=0.4 extends past the right and bottom
  // edges. The out-of-bounds region of the output must be exactly 0.0f, and
  // the call must not read outside the buffer (run under -fsanitize=address).
}

TEST(Warp, QuarterTurnRotationMapsCornersCorrectly) {
  // rotation = pi/2 on a source with a single bright pixel in one corner:
  // the bright pixel appears in the rotated-expected output corner.
}

TEST(Warp, OutputRgbFalseSwapsChannelOrder) {
  // Bgr8 source with output_rgb true vs false must differ in planes 0 and 2
  // only. This is the structural replacement for spec defect 6: the format
  // travels with the data, so no layer has to guess.
}

TEST(Warp, CropToFrameRoundTripsWarpCenter) {
  // CropToFrame(roi, {0.5f, 0.5f}) == roi.center, for a rotated, non-square roi.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R Warp --output-on-failure`
Expected: compile failure — `WarpToTensorNchw` undefined.

- [ ] **Step 3: Implement `WarpToTensorNchw` and `CropToFrame`**

For each output pixel, map crop-space `0..1` through the rect's rotation and scale into source pixel coordinates and sample bilinearly; zero when any contributing sample falls outside. Address rows via `stride`, never `width * 3`.

- [ ] **Step 4: Run tests to verify they pass, including under ASan**

Run: `ctest --test-dir cpp/build -R Warp --output-on-failure`, then reconfigure with `-DCMAKE_CXX_FLAGS=-fsanitize=address` and re-run.
Expected: all PASS, no ASan report.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/warp.hpp cpp/core/src/warp.cpp cpp/tests/test_warp.cpp
git commit -m "feat: rotated-crop bilinear resampler into NCHW tensor"
```

---

### Task 5: SSD anchor generation

**Files:**
- Create: `cpp/core/include/hc/anchors.hpp`
- Create: `cpp/core/src/anchors.cpp`
- Test: `cpp/tests/test_anchors.cpp`

**Interfaces:**
- Consumes: `cpp/docs/models.md` from Task 1 for every parameter value.
- Produces:
  - `struct AnchorOptions { int input_w, input_h; int num_layers; float min_scale, max_scale; float anchor_offset_x, anchor_offset_y; std::vector<int> strides; bool fixed_anchor_size; };`
  - `struct Anchor { float x_center, y_center, w, h; };`
  - `std::vector<Anchor> GenerateAnchors(const AnchorOptions& o);`
  - `AnchorOptions PalmAnchorOptions();` — returns the values recorded in `models.md`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Anchors, PalmAnchorCountMatchesModelOutputRows) {
  // The count recorded in models.md, which must equal the palm model's
  // regressor output row count. Take the number from models.md, not from here.
  EXPECT_EQ(GenerateAnchors(PalmAnchorOptions()).size(), kAnchorCountFromModelsMd);
}

TEST(Anchors, FirstAnchorIsCenteredInFirstCell) {
  // stride s over a square input: anchor[0].x_center == 0.5/(input_w/s),
  // likewise y. With fixed_anchor_size, w == h == 1.0.
}

TEST(Anchors, AnchorsAreGroupedByStrideInOrder) {
  // All anchors from the finest stride precede those from coarser strides,
  // matching the model's output row order. If this is wrong every decoded
  // detection lands in the wrong place, so pin it.
}

TEST(Anchors, DuplicateStridesProduceDistinctAnchorsPerLayer) {
  // The palm options repeat stride 16 across several layers; those layers must
  // each contribute their own anchors rather than collapsing into one.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R Anchors --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement `GenerateAnchors` and `PalmAnchorOptions`**

Follow MediaPipe's `SsdAnchorsCalculator` layer-walking order: group consecutive layers that share a stride, emit one anchor per aspect ratio per grid cell, row-major.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R Anchors --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/anchors.hpp cpp/core/src/anchors.cpp cpp/tests/test_anchors.cpp
git commit -m "feat: SSD anchor generation for palm detection"
```

---

### Task 6: Palm detection decoding and NMS

**Files:**
- Create: `cpp/core/include/hc/palm_decode.hpp`
- Create: `cpp/core/src/palm_decode.cpp`
- Test: `cpp/tests/test_palm_decode.cpp`

**Interfaces:**
- Consumes: `Anchor`, `Rect`, `Point2`, `IoU`, and the thresholds from `models.md`.
- Produces:
  - `struct PalmDetection { Rect box; std::array<Point2, 7> keypoints; float score; };` — all coordinates normalized to the detector's input square.
  - `struct PalmDecodeOptions { int input_w, input_h; float x_scale, y_scale, w_scale, h_scale; float min_score; float score_clipping_thresh; float nms_iou; int max_results; };`
  - `PalmDecodeOptions PalmDecodeDefaults();` — the values from `models.md`.
  - `std::vector<PalmDetection> DecodePalmDetections(const float* raw_boxes, const float* raw_scores, const std::vector<Anchor>& anchors, const PalmDecodeOptions& o);` — sorted by descending score.

- [ ] **Step 1: Write the failing tests**

```cpp
// Helper: build raw_boxes/raw_scores arrays sized for a small synthetic anchor
// set, all scores at a large negative logit except chosen indices.
TEST(PalmDecode, SinglePeakDecodesToExpectedBox) {
  // One anchor with score logit +4 and zero box offsets decodes to a box
  // centered on that anchor, with score == sigmoid(4).
}

TEST(PalmDecode, BelowThresholdYieldsNothing) {
  // All logits at -4 (sigmoid ~0.018) with min_score from models.md: empty.
}

TEST(PalmDecode, OverlappingDetectionsCollapseToOne) {
  // Two adjacent anchors both above threshold with heavily overlapping boxes:
  // exactly one survives, and it is the higher-scoring one.
}

TEST(PalmDecode, DisjointDetectionsBothSurvive) {
  // Two above-threshold anchors far apart: both returned, highest score first.
}

TEST(PalmDecode, ScoreClippingPreventsSigmoidOverflow) {
  // A logit of 1e9 must produce score 1.0 and no NaN.
}

TEST(PalmDecode, SevenKeypointsAreDecodedRelativeToAnchor) {
  // Nonzero keypoint offsets land at anchor_center + offset/scale.
}

TEST(PalmDecode, RespectsMaxResults) { /* more peaks than max_results */ }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R PalmDecode --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement `DecodePalmDetections`**

Decode each row as centre/size offsets against its anchor divided by the scale factors, clip the logit to `±score_clipping_thresh` before the sigmoid, threshold, sort descending, then suppress by `IoU` above `nms_iou`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R PalmDecode --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/palm_decode.hpp cpp/core/src/palm_decode.cpp cpp/tests/test_palm_decode.cpp
git commit -m "feat: palm detection decoding with weighted NMS"
```

---

### Task 7: ROI derivation

**Files:**
- Create: `cpp/core/include/hc/roi.hpp`
- Create: `cpp/core/src/roi.cpp`
- Test: `cpp/tests/test_roi.cpp`

**Interfaces:**
- Consumes: `PalmDetection`, `RotatedRect`, `SquareLong`, `ExpandAndShift`, and the transform constants from `models.md`.
- Produces:
  - `struct RoiOptions { float scale_x, scale_y, shift_x, shift_y; bool square_long; int rotation_kp_a, rotation_kp_b; float rotation_offset_rad; };`
  - `RoiOptions PalmRoiOptions();` and `RoiOptions LandmarkRoiOptions();` — the two different constant sets from `models.md`.
  - `RotatedRect RoiFromPalm(const PalmDetection& d, float aspect, const RoiOptions& o);`
  - `RotatedRect RoiFromLandmarks(const std::array<Point2, 21>& lm, float aspect, const RoiOptions& o);`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Roi, VerticalHandGivesZeroRotation) {
  // Rotation keypoints stacked vertically (wrist below the MCP) -> rotation
  // within 1e-3 of the configured rotation_offset_rad.
}

TEST(Roi, RotatingInputRotatesRoi) {
  // Rotate all landmarks 90 degrees about 0.5,0.5 (aspect 1) -> the ROI
  // rotation advances by pi/2 and the center follows.
}

TEST(Roi, RoiIsSquareInPixelsWhenSquareLongSet) {
  // aspect 16/9 -> roi.h / roi.w == 16/9 so the crop is square in pixels.
}

TEST(Roi, ExpansionMatchesConfiguredScale) {
  // A landmark bounding box of known size scales by scale_x/scale_y.
}

TEST(Roi, PalmAndLandmarkOptionsDifferAndBothApply) {
  // Same geometry through both option sets yields different rects: a regression
  // guard against wiring one constant set into both paths.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R Roi --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement both ROI functions**

Compute rotation from the angle between the two configured keypoint/landmark indices plus `rotation_offset_rad`, take the bounding box of the points in the rotated frame, then apply `SquareLong` and `ExpandAndShift`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R Roi --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/roi.hpp cpp/core/src/roi.cpp cpp/tests/test_roi.cpp
git commit -m "feat: ROI derivation from palm detections and landmarks"
```

---

### Task 8: Single-hand ONNX tracker

The first task where the real models run inside the real pipeline. Scoped to one hand so that the detect/track state machine is proven before two-hand association is added on top.

**Files:**
- Create: `cpp/core/include/hc/hand_tracker.hpp`
- Create: `cpp/core/include/hc/hand_tracker_onnx.hpp`
- Create: `cpp/core/src/hand_tracker_onnx.cpp`
- Create: `cpp/tests/fixture_image.hpp`
- Test: `cpp/tests/test_hand_tracker.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–7, plus `HC_PALM_MODEL` and `HC_LANDMARK_MODEL` from Task 1.
- Produces:
  - `class IHandTracker { public: virtual ~IHandTracker() = default; virtual void Process(const ImageView& frame, std::int64_t timestamp_us, std::vector<HandObservation>& out) = 0; virtual void Reset() = 0; };` — `out` is cleared and refilled, so callers can reuse one vector and avoid per-frame allocation.
  - `struct HandTrackerConfig { std::string palm_model_path; std::string landmark_model_path; int max_hands = 2; float min_presence = 0.5f; int intra_op_threads = 1; };`
  - `class HandTrackerOnnx : public IHandTracker { public: HandTrackerOnnx(const HandTrackerConfig&, Metrics&); /* IHandTracker */ };`
  - In `cpp/tests/fixture_image.hpp`: `struct OwnedImage { std::vector<std::uint8_t> pixels; ImageView view; };`, `OwnedImage LoadRawFixture(const char* path_stem);` reading the `.raw`/`.txt` pair from Task 1 with `fread`, `OwnedImage SolidImage(int w, int h, std::uint8_t gray);` for the no-hand tests, and `HandTrackerConfig DefaultTestConfig(int max_hands);` filling the model paths from compile definitions set by CMake off `HC_PALM_MODEL` and `HC_LANDMARK_MODEL`. Tasks 8 and 10 both use these.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(HandTracker, FindsOneHandInFixture) {
  Metrics m; const auto img = LoadRawFixture("hand_rgb");
  HandTrackerOnnx t(DefaultTestConfig(/*max_hands=*/1), m);
  std::vector<HandObservation> obs; t.Process(img.view, 0, obs);
  ASSERT_EQ(obs.size(), 1u);
  for (const auto& p : obs[0].landmarks) {
    EXPECT_GE(p.x, 0.0f); EXPECT_LE(p.x, 1.0f);
    EXPECT_GE(p.y, 0.0f); EXPECT_LE(p.y, 1.0f);
  }
  EXPECT_GT(obs[0].presence, 0.5f);
  EXPECT_NE(obs[0].handedness, Handedness::Unknown);
}

TEST(HandTracker, LandmarksAreNotAllIdentical) {
  // Guards the failure where the warp or the output mapping collapses every
  // landmark onto one point, which still satisfies the range assertions above.
}

TEST(HandTracker, SkipsPalmDetectorWhileTracking) {
  // Process the same fixture 5 times. Metrics::Get(Stage::PalmDetect).calls
  // must be 1, and Get(Stage::Landmark).calls must be 5. This is both the
  // accuracy and the performance mechanism in the spec, so pin it.
}

TEST(HandTracker, EmptyFrameYieldsNoObservationsAndNoError) {  // Review Focus 2
  // A mid-gray image with no hand: obs.empty(), no throw. Spec defect 2.
}

TEST(HandTracker, ReDetectsAfterHandDisappears) {
  // fixture, then blank, then fixture: PalmDetect.calls == 2 and the final
  // Process returns one observation again.
}

TEST(HandTracker, ResetClearsTrackingState) {
  // fixture, Reset(), fixture -> PalmDetect.calls == 2.
}

TEST(HandTracker, PopulatesTimestampFromArgument) { /* timestamp_us round-trips */ }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R HandTracker --output-on-failure`
Expected: compile failure — `HandTrackerOnnx` undefined.

- [ ] **Step 3: Implement `HandTrackerOnnx` for a single hand**

Hold one `Ort::Env` and two `Ort::Session`s. Per frame: if no hand is tracked, warp the whole frame to the palm model's input, run it, decode, and derive the ROI with `PalmRoiOptions()`; otherwise reuse the ROI derived from the previous frame's landmarks with `LandmarkRoiOptions()`. Warp that ROI to the landmark model's input, run it, map the 21 landmarks back through `CropToFrame`, and drop the track when `presence < min_presence`. Wrap each stage in `ScopedStage` and `Bump(Stage::PalmDetect)` on each detector invocation. Set `intra_op_threads` from the config rather than leaving the default, so the measurement task can vary it.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R HandTracker --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Confirm the unverified constants from Task 1**

If any ROI or anchor constant in `models.md` was marked as needing confirmation, this task's passing end-to-end test is that confirmation. Update `models.md` to remove the marker, or correct the value if the test forced a change.

- [ ] **Step 6: Commit**

```bash
git add cpp/core/include/hc/hand_tracker.hpp cpp/core/include/hc/hand_tracker_onnx.hpp cpp/core/src/hand_tracker_onnx.cpp cpp/tests/fixture_image.hpp cpp/tests/test_hand_tracker.cpp cpp/docs/models.md
git commit -m "feat: single-hand ONNX tracker with detect/track state machine"
```

---

### Task 9: Track association and detection de-duplication

Pure logic, no models. The spec is explicit that the landmark model's handedness output is computed per-crop and can flip, so spatial continuity is the primary association signal and handedness is only a cross-check.

**Files:**
- Create: `cpp/core/include/hc/association.hpp`
- Create: `cpp/core/src/association.cpp`
- Test: `cpp/tests/test_association.cpp`

**Interfaces:**
- Consumes: `Rect`, `RotatedRect`, `IoU`, `Handedness`, `PalmDetection`.
- Produces:
  - `struct TrackSlot { std::uint32_t id = 0; RotatedRect roi{}; Handedness handedness = Handedness::Unknown; int misses = 0; bool active = false; };`
  - `Rect AxisAlignedBounds(const RotatedRect& r);`
  - `int MatchSlotForDetection(const std::vector<TrackSlot>& slots, const PalmDetection& d, float iou_threshold);` — returns the index of the best-overlapping active slot, or `-1`.
  - `std::vector<int> AssignDetectionsToSlots(const std::vector<TrackSlot>& slots, const std::vector<PalmDetection>& dets, float iou_threshold);` — result index is the detection, value is the slot index or `-1` for "unmatched, needs a new slot". Greedy by descending IoU, one detection per slot and one slot per detection.
  - `std::uint32_t NextTrackId();` — monotonically increasing, never reuses an id.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Association, DetectionOverlappingTrackedSlotMatchesIt) {  // Review Focus 3
  // One active slot; a detection covering nearly the same area matches it,
  // rather than being reported as a new hand. This is the "palm detector
  // returns an already-tracked hand" case the spec warns about.
}

TEST(Association, DisjointDetectionIsUnmatched) { /* returns -1 */ }

TEST(Association, TwoHandsKeepTheirOwnSlots) {
  // Two slots, left at x~0.25 and right at x~0.75; two detections in the same
  // places assign to the matching slots, not crosswise.
}

TEST(Association, CrossingHandsDoNotSwapSlots) {  // Review Focus 3
  // Slots at 0.45 and 0.55 with detections at 0.46 and 0.54: each detection
  // takes the nearer slot. Assert the exact assignment, both orderings of the
  // detection vector, so the result does not depend on input order.
}

TEST(Association, OneSlotTakesOnlyOneDetection) {
  // Two overlapping detections against a single slot: one matches, the other
  // returns -1 rather than both claiming the slot.
}

TEST(Association, HandednessDisagreementDoesNotBreakSpatialMatch) {
  // A slot marked Left and a strongly overlapping detection still match.
  // Pins the spec's rule that handedness is a cross-check, not the key.
}

TEST(Association, InactiveSlotsAreNeverMatched) { /* active == false skipped */ }
TEST(Association, TrackIdsAreNeverReused) { /* three NextTrackId() calls differ */ }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R Association --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement the association functions**

Greedy assignment over all (slot, detection) pairs sorted by descending IoU, marking both sides consumed on each accepted pair. The ordering-independence test pins that the sort is total rather than dependent on input order.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R Association --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/core/include/hc/association.hpp cpp/core/src/association.cpp cpp/tests/test_association.cpp
git commit -m "feat: spatial track association and detection de-duplication"
```

---

### Task 10: Two-hand tracking

**Files:**
- Modify: `cpp/core/src/hand_tracker_onnx.cpp`
- Modify: `cpp/core/include/hc/hand_tracker_onnx.hpp`
- Modify: `cpp/tests/fixture_image.hpp`
- Test: `cpp/tests/test_hand_tracker_two.cpp`

**Interfaces:**
- Consumes: Task 8's `HandTrackerOnnx`, Task 9's association functions.
- Produces: `HandTrackerOnnx` honouring `max_hands = 2` with one `TrackSlot` per hand; `HandObservation::track_id` stable across frames for the same physical hand. Adds to `fixture_image.hpp`: `OwnedImage SideBySide(const OwnedImage& a, const OwnedImage& b);` — composites two fixtures into one double-width image, and `OwnedImage MirrorX(const OwnedImage&);` so the second hand reads as the opposite handedness.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(HandTrackerTwo, FindsTwoHandsInSideBySideFixture) {
  // SideBySide(hand, MirrorX(hand)) with max_hands=2 -> obs.size() == 2 and
  // the two track_ids differ.
}

TEST(HandTrackerTwo, TrackIdsAreStableAcrossFrames) {
  // Process the same composite 5 times; the set of track_ids is identical
  // every frame, and PalmDetect.calls == 1.
}

TEST(HandTrackerTwo, LandmarkRunsOncePerTrackedHand) {
  // After 5 frames with two hands: Landmark.calls == 10. This is the doubled
  // inference cost the spec says is the dominant phase 1 factor; measure it
  // rather than assume it.
}

TEST(HandTrackerTwo, LosingOneHandKeepsTheOther) {
  // Composite, then an image with only the left half populated: one
  // observation survives with its original track_id, the other is gone, and
  // the surviving hand's id did not change.
}

TEST(HandTrackerTwo, DetectorRunsOnlyForTheMissingHand) {
  // From the one-hand state, restore the composite: exactly one additional
  // PalmDetect call, and the still-tracked hand keeps its id.
}

TEST(HandTrackerTwo, NeverExceedsMaxHands) {
  // A three-hand composite with max_hands=2 returns exactly 2 observations.
}

TEST(HandTrackerTwo, MaxHandsOneStillWorks) {
  // Task 8's behavior is unchanged by this task: regression guard.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R HandTrackerTwo --output-on-failure`
Expected: FAIL — one observation returned where two are expected.

- [ ] **Step 3: Generalize `HandTrackerOnnx` to a slot vector**

Replace the single tracked ROI with `std::vector<TrackSlot>` sized to `max_hands`. Per frame: run the landmark model for each active slot from its own previous-landmark ROI; if any slot is inactive, run the palm detector once, assign detections with `AssignDetectionsToSlots`, discard detections matching an already-active slot, and fill free slots with the remainder up to `max_hands`. Deactivate a slot when its presence falls below `min_presence`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build -R HandTracker --output-on-failure`
Expected: all PASS, including Task 8's tests.

- [ ] **Step 5: Commit**

```bash
git add cpp/core cpp/tests
git commit -m "feat: two-hand tracking with per-hand ROI state"
```

---

### Task 11: Portable frame sources

Synthetic and threaded sources live in core so the pipeline is testable with no camera and no OpenCV, which the spec makes a hard requirement.

**Files:**
- Create: `cpp/core/include/hc/frame_source.hpp`
- Create: `cpp/core/include/hc/synthetic_frame_source.hpp`
- Create: `cpp/core/src/synthetic_frame_source.cpp`
- Create: `cpp/core/include/hc/latest_frame_source.hpp`
- Create: `cpp/core/src/latest_frame_source.cpp`
- Test: `cpp/tests/test_frame_source.cpp`

**Interfaces:**
- Consumes: `ImageView` from Task 2.
- Produces:
  - `struct Frame { ImageView view; std::int64_t timestamp_us; std::uint64_t index; };`
  - `class IFrameSource { public: virtual ~IFrameSource() = default; virtual bool Next(Frame& out) = 0; virtual void Close() = 0; };` — `Next` returns `false` once and forever when the source has ended or failed.
  - `class SyntheticFrameSource : public IFrameSource { public: SyntheticFrameSource(int width, int height, int frame_count); };` — `frame_count < 0` means unlimited; emits a moving pattern so successive frames differ.
  - `class LatestFrameSource : public IFrameSource { public: explicit LatestFrameSource(std::unique_ptr<IFrameSource> inner); ~LatestFrameSource(); std::uint64_t dropped() const; };` — runs `inner` on its own thread and always hands out the newest frame, counting the ones skipped. The returned `ImageView` stays valid until the next `Next` call.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(FrameSource, SyntheticYieldsRequestedCountThenEnds) {
  // frame_count 3: three Next() true with index 0,1,2 then false, then false
  // again on a further call.
}

TEST(FrameSource, SyntheticFramesDiffer) { /* frame 0 and frame 1 pixels differ */ }

TEST(FrameSource, LatestForwardsEveryFrameWhenConsumerKeepsUp) {
  // Unlimited synthetic source, consume 10 frames with no delay: indices are
  // non-decreasing and at least one frame arrives.
}

TEST(FrameSource, LatestDropsStaleFramesWhenConsumerIsSlow) {
  // Sleep between Next() calls against a fast source: dropped() > 0 and the
  // index jumps by more than one. Dropping is the intended behavior, not a bug.
}

TEST(FrameSource, LatestEndsWhenInnerSourceEnds) {  // Review Focus 4
  // A 3-frame inner source: Next() eventually returns false and keeps
  // returning false. Must not block forever waiting for a frame that is
  // never coming.
}

TEST(FrameSource, LatestSurvivesInnerFailureOnFirstFrame) {  // Review Focus 4
  // An inner source whose Next() returns false immediately — the phone-camera
  // -never-connected case. Construction plus Next() must return false
  // promptly, not hang and not spin. Assert the call returns within a bound.
}

TEST(FrameSource, LatestDestructorJoinsCleanly) {
  // Destroy while the capture thread is mid-flight, under TSan: no leak, no
  // use-after-free, no hang.
}

TEST(FrameSource, CloseIsIdempotent) { /* two Close() calls, then Next() false */ }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build -R FrameSource --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement both sources**

`LatestFrameSource` keeps two buffers and a mutex plus condition variable: the thread fills the back buffer and swaps, `Next` waits for a new frame or for the ended flag. The two Review Focus tests pin that the ended flag wakes every waiter.

- [ ] **Step 4: Run tests to verify they pass, including under TSan**

Run: `ctest --test-dir cpp/build -R FrameSource --output-on-failure`, then reconfigure with `-DCMAKE_CXX_FLAGS=-fsanitize=thread` and re-run.
Expected: all PASS, no TSan report.

- [ ] **Step 5: Commit**

```bash
git add cpp/core cpp/tests/test_frame_source.cpp
git commit -m "feat: synthetic and latest-frame portable frame sources"
```

---

### Task 12: OpenCV camera and video-file frame source

The first task that needs OpenCV, so the first run with `-DHC_BUILD_APP=ON` and a long first configure.

**Files:**
- Create: `cpp/cmake/FetchOpenCV.cmake`
- Create: `cpp/app/CMakeLists.txt`
- Create: `cpp/app/src/opencv_frame_source.hpp`
- Create: `cpp/app/src/opencv_frame_source.cpp`
- Test: `cpp/tests/test_opencv_frame_source.cpp`

**Interfaces:**
- Consumes: `IFrameSource`, `Frame`, `ImageView` from Task 11.
- Produces:
  - `struct CameraOptions { int device_index = 0; int width = 1280; int height = 720; bool prefer_mjpeg = true; };`
  - `class OpenCvFrameSource : public IFrameSource { public: static std::unique_ptr<OpenCvFrameSource> OpenCamera(const CameraOptions&); static std::unique_ptr<OpenCvFrameSource> OpenFile(const std::string& path); };` — returns `nullptr` on failure rather than throwing. Exposes `std::string DescribeFormat() const;` reporting the fourcc and resolution actually negotiated.

- [ ] **Step 1: Write `cpp/cmake/FetchOpenCV.cmake`**

`FetchContent` from the OpenCV source release, pinned by `URL_HASH`, with `BUILD_LIST` restricted to `core,imgproc,imgcodecs,videoio,highgui` and the test, example, apps, Python and Java builds disabled. Guard the whole file behind `HC_BUILD_APP`.

- [ ] **Step 2: Write the failing tests**

```cpp
TEST(OpenCvFrameSource, OpenFileOnMissingPathReturnsNull) {
  EXPECT_EQ(OpenCvFrameSource::OpenFile("does-not-exist.mp4"), nullptr);
}

TEST(OpenCvFrameSource, OpenCameraOnAbsentDeviceReturnsNull) {
  // There is no camera on this machine, so this is the behavior that actually
  // runs here: a clean nullptr, not a throw and not a hang.
  CameraOptions o; o.device_index = 99;
  EXPECT_EQ(OpenCvFrameSource::OpenCamera(o), nullptr);
}

TEST(OpenCvFrameSource, ReadsEveryFrameOfAGeneratedVideo) {
  // Write a short video with cv::VideoWriter in the test's own temp dir, read
  // it back: frame count matches, each view has width/height/stride set and
  // stride >= width*3.
}

TEST(OpenCvFrameSource, ReportsFormatForAFileSource) {
  // DescribeFormat() is non-empty and names the resolution.
}

TEST(OpenCvFrameSource, ViewFormatIsBgrForOpenCvFrames) {
  // OpenCV decodes to BGR; the view must say so rather than claim Rgb8. A
  // wrong format here is spec defect 6 reintroduced.
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cmake -S cpp -B cpp/build-app -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DHC_BUILD_APP=ON && cmake --build cpp/build-app && ctest --test-dir cpp/build-app -R OpenCvFrameSource`
Expected: compile failure. The first configure builds OpenCV and takes a long time.

- [ ] **Step 4: Implement `OpenCvFrameSource`**

For the camera path set `CAP_PROP_FOURCC` to MJPG when `prefer_mjpeg`, then width, height, and `CAP_PROP_BUFFERSIZE` to 1 — the spec names these the largest real throughput lever in the project. Read back what was negotiated for `DescribeFormat`. Expose the decoded `cv::Mat` as an `ImageView` with its real `step` as the stride and `PixelFormat::Bgr8`, with no colour conversion.

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build-app -R OpenCvFrameSource --output-on-failure`
Expected: all PASS.

- [ ] **Step 6: Re-verify the layering guard still holds**

Run: `cmake -S cpp -B cpp/build-app -G Ninja -DHC_BUILD_APP=ON`
Expected: configure succeeds, meaning `hc_core` still links no OpenCV even though the app now does.

- [ ] **Step 7: Commit**

```bash
git add cpp/cmake/FetchOpenCV.cmake cpp/app cpp/tests/test_opencv_frame_source.cpp cpp/CMakeLists.txt
git commit -m "feat: OpenCV camera and file frame source with MJPEG negotiation"
```

---

### Task 13: Debug overlay

**Files:**
- Create: `cpp/app/src/overlay.hpp`
- Create: `cpp/app/src/overlay.cpp`
- Test: `cpp/tests/test_overlay.cpp`

**Interfaces:**
- Consumes: `ImageView`, `HandObservation`.
- Produces:
  - `class Overlay { public: explicit Overlay(std::string window_title); void Render(const ImageView& frame, const std::vector<HandObservation>& obs, const std::string& hud); bool ShouldQuit(); };` — `ShouldQuit` returns true on Escape or when the window is closed.
  - `void DrawObservations(cv::Mat& bgr, const std::vector<HandObservation>& obs);` — the pure drawing half, separated so it is testable without a window. **This is the only place in the project that multiplies normalized coordinates by pixel dimensions.**

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Overlay, DrawsLandmarksAtPixelPositionsFromNormalizedCoords) {
  // A black 640x480 Mat and one observation with a landmark at {0.5f, 0.25f}:
  // after DrawObservations, a nonzero pixel exists within a few px of
  // (320, 120). Pins the single normalized-to-pixel conversion in the project.
}

TEST(Overlay, DrawsBothHandsInDistinctColors) {
  // Two observations with different track_ids produce two different nonzero
  // colors, so the two hands are distinguishable on screen.
}

TEST(Overlay, EmptyObservationsLeaveFrameUnchanged) {
  // DrawObservations with an empty vector does not modify a single pixel.
}

TEST(Overlay, HandlesLandmarksOutsideUnitRangeWithoutCrashing) {
  // A landmark at {1.4f, -0.2f} must be clipped, not written out of bounds.
  // Run under ASan.
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build-app -R Overlay --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement `DrawObservations` and `Overlay`**

Draw a circle per landmark and the skeleton connections, colour-keyed by `track_id`, plus the `hud` string. `Render` wraps the `ImageView` as a `cv::Mat`, clones it, draws, and calls `imshow` and `waitKey(1)`. `ShouldQuit` checks the key and `getWindowProperty(..., WND_PROP_VISIBLE)` — the Python version's quit condition, which worked and is worth keeping.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir cpp/build-app -R Overlay --output-on-failure`
Expected: all PASS. Only `DrawObservations` is exercised; `Render` needs a display and is covered by Task 14's manual run.

- [ ] **Step 5: Commit**

```bash
git add cpp/app/src/overlay.hpp cpp/app/src/overlay.cpp cpp/tests/test_overlay.cpp
git commit -m "feat: debug overlay rendering normalized landmarks to pixels"
```

---

### Task 14: Application shell and the measurement report

The phase deliverable. The spec is explicit that the report breaks down per stage rather than giving one FPS number, and that phase 1 then **stops**.

**Files:**
- Create: `cpp/app/src/main.cpp`
- Create: `cpp/README.md`
- Create: `docs/superpowers/reports/2026-11-12-phase1-measurements.md`
- Modify: `cpp/app/CMakeLists.txt`
- Test: `cpp/tests/test_cli.cpp`

**Interfaces:**
- Consumes: every preceding task.
- Produces: a `hand_controller` executable accepting `--camera N`, `--video PATH`, `--synthetic WxH`, `--max-hands N` (default 2), `--frames N` (0 = until quit), `--headless`, `--threads N`, `--report PATH`. Also `struct CliOptions` and `bool ParseCli(int argc, char** argv, CliOptions& out, std::string& error);` in a header so parsing is testable without running the app.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Cli, DefaultsToTwoHands) { /* no args -> max_hands == 2 */ }
TEST(Cli, ParsesSyntheticGeometry) { /* --synthetic 640x360 -> 640, 360 */ }
TEST(Cli, RejectsMalformedSyntheticGeometry) { /* "--synthetic 640" -> false, error non-empty */ }
TEST(Cli, RejectsTwoSourcesAtOnce) {
  // --camera 0 together with --video a.mp4 -> false with a message naming the
  // conflict, rather than silently preferring one.
}
TEST(Cli, RejectsUnknownFlag) { /* --nope -> false, error names the flag */ }
TEST(Cli, HeadlessImpliesNoOverlay) { /* --headless -> want_overlay == false */ }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir cpp/build-app -R Cli --output-on-failure`
Expected: compile failure.

- [ ] **Step 3: Implement `ParseCli` and `main`**

`main` builds the chosen source, wraps it in `LatestFrameSource`, constructs `HandTrackerOnnx` with `Metrics`, and loops: `Next`, `Process`, optional `Render`, until the frame budget is spent or the user quits. Time the `Next` call as `Stage::CaptureWait` and the whole iteration as `Stage::Total`. On exit, write `Metrics::Format()` to stdout and to `--report` when given.

- [ ] **Step 4: Run the CLI tests and a synthetic end-to-end run**

Run: `ctest --test-dir cpp/build-app -R Cli --output-on-failure`, then `./cpp/build-app/app/hand_controller --synthetic 1280x720 --frames 300 --headless`
Expected: tests PASS; the run completes and prints a report with a row per stage.

- [ ] **Step 5: Measure on real frames**

Record a short video of two hands (or have the owner supply one from their DroidCam setup) and run with `--video`, `--frames 300`, both `--max-hands 1` and `--max-hands 2`, and `--threads 1` and `--threads 4`.

- [ ] **Step 6: Write the measurement report**

In `docs/superpowers/reports/2026-11-12-phase1-measurements.md`: the machine (i5-12400, 12 threads), the ONNX Runtime version, the model variants, and a table of mean/p50/p95 per stage for each configuration above. State the end-to-end latency and the achieved frame rate. Note explicitly how much of the per-frame cost the second hand adds, since the spec names that the dominant open question. Draw no conclusion about whether to proceed and start no optimization work — the spec reserves that decision for the owner.

- [ ] **Step 7: Write `cpp/README.md`**

The build commands for both `-DHC_BUILD_APP=OFF` (fast, core and tests only) and `ON`, the CLI flags, and a note that the Windows backends compile but are unverified.

- [ ] **Step 8: Commit and stop**

```bash
git add cpp/app cpp/README.md cpp/tests/test_cli.cpp docs/superpowers/reports
git commit -m "feat: application shell and phase 1 measurement report"
```

Then report the numbers to the owner and stop. Phase 2 is a separate plan.

---

## Notes on decisions this plan makes

- **The rotated-crop resampler is hand-written** (Task 4) rather than using `cv::warpAffine`, because `hc_core` may not link OpenCV. It also removes the need for any full-frame colour conversion: one warp reads from the source frame straight into the model's input tensor in the right channel order and size, which is the structural fix for spec defect 6 and satisfies the spec's "inference runs on a downscaled copy" requirement without a separate downscale step.
- **`RotatedRect::w` is normalized to frame width and `h` to frame height**, so a square-in-pixels ROI is not square in these units. Every function that needs squareness takes an `aspect` parameter. The alternative — storing both in width-relative units — hides the asymmetry and gets silently wrong on non-16:9 cameras.
- **`HC_BUILD_APP=OFF` is the default build mode for Tasks 2–11.** OpenCV from source is the accepted cost of the owner's self-contained-build decision, but nothing before Task 12 needs it, and paying it on every test cycle would be the plan's own fault rather than the decision's.
- **GoogleTest via `FetchContent`**, consistent with the clone-plus-cmake constraint. The spec left the test framework to the builder.
- **Task 1 records model parameters rather than this plan asserting them.** Anchor counts, score thresholds and ROI constants live inside MediaPipe's graph configuration, not in the model files, and a plan that hardcoded guesses for them would propagate those guesses into seven downstream tasks. Task 8's end-to-end test is what confirms the ones that cannot be read off the files.
