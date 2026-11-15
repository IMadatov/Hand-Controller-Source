# hand_controller (C++)

The phase 1 C++ rebuild of the hand-tracking pipeline: MediaPipe's palm-detect
plus hand-landmark models on ONNX Runtime, two-hand tracking, a debug overlay,
and per-stage timing instrumentation.

Phase 1 ends at measurement. It tracks hands and reports where the time goes;
it does **not** inject input events or draw. See
`docs/superpowers/specs/2026-11-12-hand-controller-cpp-design.md`.

## Layout

| Path | Target | What it is |
|---|---|---|
| `base/` | `hc_base` | Primitives: geometry, image types, metrics, frame sources. **No ONNX**, no OS |
| `vision/` | `hc_vision` | The hand-detection module: anchors, palm decode, ROI, warp, association, features, `IHandTracker`. **No ONNX** |
| | `hc_vision_ort` | The ONNX Runtime backend — the only ORT consumer in the project |
| `platform/` | `hc_platform` | The per-system layer. Frames in, pixels out. The **only** target allowed OpenCV |
| `app/` | `hc_cli` | Argument parsing. Links nothing but the standard library |
| | `hand_controller` | `main()`, which wires every layer together |
| `tests/` | | GoogleTest suites, one binary per unit |
| `tools/` | `validate_models` | Produced `docs/models.md` |

`control/` (`hc_control`: gesture recognition and mapping) arrives with the
phase 2 gesture tasks.

### Why it is split this way

The dependency order is one-directional:

```
hc_base  <-  hc_vision  <-  hc_vision_ort
    ^            ^                ^
    |            |                |
    +--------- hc_platform -------+---- hand_controller ---- hc_cli
```

`hc_vision` is the standalone hand-detection module. It does not know what a
gesture is, where frames come from, or how anything is drawn — it turns pixels
into normalized landmarks and stops. Keeping it free of ONNX Runtime is what
makes a second backend (ORT Web for WebAssembly, NNAPI, Core ML) a sibling
target implementing the same `IHandTracker` rather than an edit to the
algorithms.

`hc_platform` is where systems differ, and it holds **no logic**. What a
gesture means and which command it maps to stays in portable code below, so
there is one copy of that decision rather than one per operating system.
Someone asking "where does Linux differ from Windows" should find the entire
answer inside `platform/`.

### The layering is enforced, not documented

`cmake/HcAssertNoForbiddenDeps.cmake` provides two assertions:

- `hc_assert_no_forbidden_deps(<target>)` — the target links no OpenCV, X11 or
  Xtst, transitively. Applied to every target below `hc_platform`.
- `hc_assert_does_not_link(<target> FORBIDDEN <names...>)` — the direction
  check, which stops a lower layer reaching up into a higher one.

Both only *register*; `hc_run_layer_checks()` at the bottom of
`CMakeLists.txt` runs them once the link graph is final. That ordering is the
whole point — a check that runs where it is written sees only the link
libraries added above it, and a violation appended below it configures
cleanly. The first version of this file had exactly that hole.

Every test links the **narrowest** target that satisfies it rather than a
convenient superset, so a unit that quietly grows a dependency on a higher
layer stops linking in `tests/CMakeLists.txt` instead of passing review.

The core speaks normalized `0.0..1.0` coordinates everywhere, and
`DrawObservations` in `platform/src/overlay.cpp` is the only place in the
project that multiplies them by pixel dimensions.

## Building

Both configurations fetch ONNX Runtime and the two ONNX models at configure
time (hash-pinned).

**Portable layers and tests only — the fast path.** No OpenCV is fetched or
built. This is what to use when working on anything under `base/`, `vision/`
or `app/src/cli.cpp`, and it covers 127 of the 145 tests:

```bash
cmake -S cpp -B cpp/build -G Ninja -DHC_BUILD_APP=OFF
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
```

**With the application.** Builds OpenCV 4.14.0 from source, which takes
20–40 minutes the first time and roughly 530 MB of disk. Subsequent builds
reuse it:

```bash
cmake -S cpp -B cpp/build-app -G Ninja -DHC_BUILD_APP=ON
cmake --build cpp/build-app
ctest --test-dir cpp/build-app --output-on-failure
```

> Do not delete `cpp/build-app` to "clean up" — it carries that OpenCV build.

Consider `-DCMAKE_BUILD_TYPE=Release`. It is **not** set by default, so a bare
configure compiles our own code at `-O0`; the phase 1 measurements were taken
that way and say so.

## Running

```bash
./cpp/build-app/bin/hand_controller [options]
```

| Flag | Meaning |
|---|---|
| `--camera N` | Capture from camera index N (default: 0, 1280x720, MJPEG) |
| `--video PATH` | Read frames from a video file |
| `--synthetic WxH` | Generate frames; needs no camera |
| `--max-hands N` | Hands to track, 1..32 (default: 2) |
| `--frames N` | Stop after N frames; 0 = until quit (default: 0) |
| `--threads N` | ONNX Runtime intra-op threads (default: 1) |
| `--headless` | No preview window |
| `--report PATH` | Also write the timing report to PATH |
| `--help` | Print usage |

The three source flags are mutually exclusive; passing two is an error rather
than one silently winning. With the overlay up, **Escape** or **q** quits, as
does closing the window.

```bash
# No camera needed:
./cpp/build-app/bin/hand_controller --synthetic 1280x720 --frames 300 --headless

# Measure a clip:
./cpp/build-app/bin/hand_controller --video clip.avi --headless \
    --max-hands 2 --threads 4 --report out.md
```

`--max-hands` is a **ceiling, not a target**, and the distinction is expensive:
the palm detector re-runs on every frame in which any slot is empty. Setting
`2` in a scene that only ever shows one hand pays a palm detection
(~8 ms) on every frame. Set it to the number of hands you mean to track.

## Measurements

`docs/superpowers/reports/2026-11-12-phase1-measurements.md`. Headline: the
second hand costs **+95%** per frame — it shares no work with the first.

## Platform support

A platform counts as supported here only once the build has actually been
configured, compiled and run on it. By that standard:

| Platform | Status |
|---|---|
| Linux x64 | **Verified.** Everything above was built and run here |
| Windows x64 | **Untested.** The CMake exists and is believed correct — `cmake/FetchOnnxRuntime.cmake` selects the hash-pinned `win-x64` ONNX Runtime archive — but it has never been configured, compiled or run. No Windows machine was available |
| macOS | **Absent.** `FetchOnnxRuntime.cmake` has no `osx` branch at all. Adding one is an archive URL, a SHA256 and an `elseif`; it is cheap but it has not been done |
| Android / iOS | **Not attempted.** C++17 was chosen for NDK compatibility and ONNX Runtime ships for both, so `hc_base`, `hc_vision` and `hc_vision_ort` should port. Frame acquisition (CameraX, AVFoundation) and rendering have no OpenCV path and would be new `hc_platform` implementations |

The portable layers carry no platform-specific code to port — that is the
point of the split. Divergence is concentrated in `platform/`, and grows when
the phase 2 injection tasks land: X11/XTest on Linux, `SendInput` on Windows,
`CGEvent` on macOS. Note that input injection does not exist on iOS at all,
and on Android only through a user-granted `AccessibilityService`.

## Notes for this machine

- **No camera** is attached, so `--camera` is untested here and `--synthetic`
  or `--video` is the way to exercise the pipeline.
- **No FFmpeg and no GStreamer.** OpenCV's built-in Motion JPEG encoder is the
  only one available, so any video fixture must be **MJPG-in-AVI**. An `.mp4`
  will not open.
- Avoid video filenames like `clip_1.avi`: OpenCV's image-sequence backend
  claims names matching a `%0Nd` pattern and may open them as a sequence of
  stills rather than as a video.
