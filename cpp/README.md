# hand_controller (C++)

The C++ rebuild of the hand-tracking pipeline: MediaPipe's palm-detect plus
hand-landmark models on ONNX Runtime, two-hand tracking with fully independent
per-hand state, configurable gesture-to-command bindings, a debug overlay, and
per-stage timing instrumentation.

Phase 1 ended at measurement. **Phase 2 added the layers above the tracker** —
gesture recognition, action mapping and output sinks — so it now decides what a
pose means and what command it should produce. What it does NOT yet do is
actually inject that command on this machine: the session is Wayland and XTest
is silently dead there, which is explained under "Gestures, bindings and input
injection" below and is not a detail to skip.

Still out of scope: draw mode, the header strip, a network sink, mobile.

Design: `docs/superpowers/specs/2026-11-12-hand-controller-cpp-design.md`.
Phase 2 outcome: `docs/superpowers/reports/2026-11-15-phase2-notes.md`.

## Layout

| Path | Target | What it is |
|---|---|---|
| `base/` | `hc_base` | Primitives: geometry, image types, metrics, frame sources. **No ONNX**, no OS |
| `vision/` | `hc_vision` | The hand-detection module: anchors, palm decode, ROI, warp, association, features, `IHandTracker`. **No ONNX** |
| | `hc_vision_ort` | The ONNX Runtime backend — the only ORT consumer in the project |
| `control/` | `hc_control` | The logic layer: gesture recognition, bindings, action mapping, output sinks. Pure — no OS, no OpenCV, no inference runtime |
| `platform/` | `hc_platform` | The per-system layer. Frames in, pixels out, events out. The **only** target allowed OpenCV |
| `app/` | `hc_cli` | Argument parsing. Links nothing but the standard library |
| | `hand_controller` | `main()`, which wires every layer together |
| `tests/` | | GoogleTest suites, one binary per unit |
| `tools/` | `validate_models` | Produced `docs/models.md` |
| | `xtest_probe` | Proves whether XTest actually moves the pointer |
| | `dump_contract` | Emits the whole UI contract as JSON, generated from `hc_control` |
| | `bench_control` | Per-frame cost of recognition, mapping and dispatch |

### Why it is split this way

The dependency order is one-directional:

```
hc_base  <-  hc_vision  <-  hc_vision_ort
    ^            ^                ^
    |            |                |
    |        hc_control           |
    |            ^                |
    +--------- hc_platform -------+---- hand_controller ---- hc_cli
```

`hc_control` links `hc_vision` for `HandObservation` and **not**
`hc_vision_ort`: gesture recognition consumes plain data and has no business
carrying an inference runtime. That is asserted at configure time, not
reviewed, and it is the precondition for a WebAssembly build that delegates
inference to JavaScript while keeping the gesture logic.

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
`control/` or `app/src/cli.cpp`, and it covers 263 of the 288 tests:

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
| `--bindings PATH` | Gesture bindings file (default: the compiled-in one) |
| `--sensitivity F` | Cursor speed multiplier, > 0; overrides the file |
| `--smoothing F` | Cursor smoothing, 0..1; overrides the file |
| `--sink NAME` | `null`, `log`, `xtest` or `sendinput` (default: **`null`**) |
| `--no-inject` | Force the null sink, whatever `--sink` says |
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

## Gestures, bindings and input injection

A pose is recognized, mapped to an abstract command, and handed to a sink. The
first two steps are portable and the third is the only one that touches the
operating system — so the same gesture means the same thing everywhere, and
only the injection differs per platform.

### Nothing is injected unless you ask

**`--sink null` is the default.** Development happens on the same desktop this
program would seize, and a build that grabs the cursor the moment it starts is
hostile to the person writing it. `--sink log` prints what the gestures *would*
have done. `--no-inject` forces null over an explicit `--sink`, in either order
on the command line.

### Wayland: XTest reports success and does nothing

> On a **Wayland** session, `XTestFakeMotionEvent` returns success and the
> cursor does not move. The extension reports itself present at version 2.2,
> `libXtst.so.6` is there, and every signal short of reading the pointer back
> says it works.

This is measured, not assumed — `cpp/tools/xtest_probe` is committed so it is
repeatable:

```
$ ./cpp/build/tools/xtest_probe
XTestQueryExtension: present=1 version=2.2
pointer before: 1920,540
XTestFakeMotionEvent returned 1 (asked for 2057,631)
pointer after:  1920,540
RESULT: XTest SILENTLY DID NOTHING (returned success, no move)
```

Consequently **`--sink xtest` is not built yet** and refuses with that
explanation rather than pretending: a sink that reports success while moving
nothing is the exact failure this project is trying not to ship. Real X11
injection needs an X11 session; Wayland needs `libei`, which is a larger piece
of work and out of scope.

### The bindings file

Flat line-oriented text — not JSON, which would mean either a fetched
dependency (forbidden here) or a hand-rolled parser larger than the layer it
configures. The shipped configuration is `config/default_bindings.txt`, and it
is **compiled into the binary**, so the program works with no file present.

```
<hand> <gesture> <phase> -> <command> [arg]

set sensitivity 2.0
set smoothing 0.5

right point_index held  -> move_cursor
right two_fingers began -> click left
left  thumb_up    began -> key_press   arrow_right
left  thumb_up    ended -> key_release arrow_right
```

- **hand** — `any` | `left` | `right`. `any` also matches an **unlabelled**
  hand, which matters: handedness confidence was measured as low as 0.575, so
  `Unknown` arrives often and `left`/`right` alone would silently ignore it.
- **gesture** — `pinch`, `fist`, `open_palm`, `point_index`, `two_fingers`,
  `thumb_up`, `pinky_only`, `four_fingers`. Each names the **pose**, never the
  action, because any pose can be bound to anything.
- **phase** — `began` fires once, `held` fires every frame, `ended` fires on
  release.
- **command** — `move_cursor`, `scroll`, `click`, `double_click`, `key_press`,
  `key_release`. `click`/`double_click` take an optional button (default
  `left`); the key commands require a key name; `move_cursor`/`scroll` take no
  argument.

Two rules are **enforced at load**, and a file breaking either is rejected
rather than half-applied:

1. No two bindings may overlap on the same gesture and phase. `any` overlaps
   `left` and `right`, so `any pinch began` plus `left pinch began` is an
   error even though the lines differ.
2. Every `key_press` must have a matching `key_release` on the `ended` phase.
   A key still down outlives the process, and a configuration is the easiest
   place to cause one.

An **empty** configuration is valid and means no bindings. It is not an error.

### One behaviour worth knowing before you tune anything

Five pairs in the vocabulary differ by a single finger — `fist`/`point_index`,
`fist`/`thumb_up`, `fist`/`pinky_only`, `open_palm`/`four_fingers`,
`point_index`/`two_fingers`. A gesture survives one finger of flicker on
purpose, so switching between two of those takes a **confirmed takeover**: the
old gesture `ended`s on one frame and the new one `began`s on a later one (two
frames by default, ~20 ms). Point to move the cursor and raise the middle
finger to click, and that is the path being taken.

### For a UI built on top of this

`cpp/tools/dump_contract` emits the whole vocabulary, the shipped
configuration, the validation rules and a real recognizer session as JSON,
generated from the code rather than transcribed from it:

```bash
cmake --build cpp/build --target dump_contract
./cpp/build/tools/dump_contract > contract.json
```

Prose version and the layering rules:
`docs/superpowers/specs/2026-11-15-gui-contract.md`.

## Measurements

`docs/superpowers/reports/2026-11-12-phase1-measurements.md`. Headline: the
second hand costs **+95%** per frame — it shares no work with the first.

Gesture recognition, mapping and dispatch together cost **~0.0013 ms per
frame** with two hands — about 0.01% of the tracker. Re-measure with
`cpp/build/tools/bench_control`.

## Platform support

A platform counts as supported here only once the build has actually been
configured, compiled and run on it. By that standard:

| Platform | Status |
|---|---|
| Linux x64 | **Verified.** Everything above was built and run here |
| Windows x64 | **Untested.** The CMake exists and is believed correct — `cmake/FetchOnnxRuntime.cmake` selects the hash-pinned `win-x64` ONNX Runtime archive — but it has never been configured, compiled or run. No Windows machine was available. The `SendInput` sink behind `--sink sendinput` is written and reachable, and has **never been compiled**: the whole file sits behind `#ifdef _WIN32`, so on Linux not even the compiler has read it. There is deliberately no test for it |
| macOS | **Absent.** `FetchOnnxRuntime.cmake` has no `osx` branch at all. Adding one is an archive URL, a SHA256 and an `elseif`; it is cheap but it has not been done |
| Android / iOS | **Not attempted.** C++17 was chosen for NDK compatibility and ONNX Runtime ships for both, so `hc_base`, `hc_vision` and `hc_vision_ort` should port. Frame acquisition (CameraX, AVFoundation) and rendering have no OpenCV path and would be new `hc_platform` implementations |

The portable layers carry no platform-specific code to port — that is the
point of the split. Divergence is concentrated in `platform/`, and grows when
the phase 2 injection tasks land: `SendInput` on Windows (written, never run),
X11/XTest on Linux (blocked — see the Wayland note above), `CGEvent` on
macOS (not started). Note that input injection does not exist on iOS at all,
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
