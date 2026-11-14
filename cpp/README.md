# hand_controller (C++)

The phase 1 C++ rebuild of the hand-tracking pipeline: MediaPipe's palm-detect
plus hand-landmark models on ONNX Runtime, two-hand tracking, a debug overlay,
and per-stage timing instrumentation.

Phase 1 ends at measurement. It tracks hands and reports where the time goes;
it does **not** inject input events or draw. See
`docs/superpowers/specs/2026-11-12-hand-controller-cpp-design.md`.

## Layout

| Path | What it is |
|---|---|
| `core/` | `hc_core` — the pipeline. Normalized coordinates only, **no OpenCV** |
| `app/` | The OpenCV layer: frame sources, overlay, CLI, `main` |
| `tests/` | GoogleTest suites, one binary per unit |
| `tools/` | `validate_models`, which produced `docs/models.md` |
| `docs/models.md` | The model record from Task 1 — observed values, not guesses |

The core/app split is enforced, not just documented: `hc_assert_no_forbidden_deps`
fails the build if anything drags OpenCV into `hc_core`. The core speaks
normalized `0.0..1.0` coordinates everywhere, and `DrawObservations` in
`app/src/overlay.cpp` is the only place in the project that multiplies them by
pixel dimensions.

## Building

Both configurations fetch ONNX Runtime and the two ONNX models at configure
time (hash-pinned).

**Core and tests only — the fast path.** No OpenCV is fetched or built. This is
what to use when working on anything under `core/`:

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

Linux is the only **verified** platform; everything above was built and run
there.

The CMake for Windows exists (`cmake/FetchOnnxRuntime.cmake` selects the
`win-x64` ONNX Runtime archive, hash-pinned) and is believed correct, but
**has never been configured, compiled, or run** — no Windows machine was
available. Treat it as untested, not as supported. There is no
platform-specific application code to port: phase 1 contains no input
injection, which is where the real platform divergence will arrive in phase 2.

## Notes for this machine

- **No camera** is attached, so `--camera` is untested here and `--synthetic`
  or `--video` is the way to exercise the pipeline.
- **No FFmpeg and no GStreamer.** OpenCV's built-in Motion JPEG encoder is the
  only one available, so any video fixture must be **MJPG-in-AVI**. An `.mp4`
  will not open.
- Avoid video filenames like `clip_1.avi`: OpenCV's image-sequence backend
  claims names matching a `%0Nd` pattern and may open them as a sequence of
  stills rather than as a video.
