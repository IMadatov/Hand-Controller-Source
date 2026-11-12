# Hand Controller — C++ rebuild design

Date: 2026-11-12
Status: approved design, ready for planning

## Intent and why

The existing `main.py` (393 lines, module-level script) turns a webcam feed into
mouse and keyboard input via MediaPipe hand landmarks, plus a second "draw" mode
that paints on a canvas overlay. It works, but it is a single flat script with
~20 module-level globals, resolution baked into the gesture logic, and no seam
anywhere that would let any part of it be reused.

The owner wants to rebuild it in C++ in a new folder inside the same repository,
structured in layers, so that the core can later be driven by different inputs
and drive different outputs — including, eventually, a mobile build.

**An explicit note on performance.** The owner's initial framing was "rebuild in
C++ for performance." That framing was examined and rejected as the primary
justification, and the owner was told so before deciding to proceed. Measured
reasoning: at 30 FPS the frame budget is 33 ms. In the Python version that budget
is consumed by `pyautogui.PAUSE` (100 ms of `time.sleep` per injected click,
scroll or keypress, fired from inside the frame loop), by the camera blocking in
a non-MJPEG pixel format, and by full-frame OpenCV compositing that runs even
when the canvas is empty. MediaPipe inference (~10–15 ms) and all OpenCV
operations are **already C++**; Python only holds the binding. The code actually
executing in the interpreter per frame — the finger-counting and the `if` chain —
is roughly 0.2 ms, about 0.6% of the budget. Porting that to C++ is not where the
frames are.

The rebuild is therefore justified on different grounds, which do hold:

- startup time and distribution size (PyInstaller: ~3–5 s and ~300 MB; native:
  near-instant and ~20 MB)
- a single native executable with no Python runtime
- no GIL, and a clean seam for a future mobile build
- a structure that can be extended and customized, which the current script
  cannot

A builder should not promise, or be measured against, an FPS improvement from
the language change alone. The FPS improvements available in this project come
from the architecture (threaded capture, MJPEG, downscaled inference, no
blocking sleeps in the loop) and would have been available in Python too.

## Goals

1. A layered C++ core where each layer can be replaced or customized
   independently, and where the layers in the middle have no knowledge of
   cameras, operating systems, or OpenCV.
2. **Phase 1 only**: `FrameSource` → `HandTracker` working end to end, producing
   hand landmarks for up to two hands, with a debug view and per-stage timing
   numbers. This phase answers "does ONNX hand tracking work, and how many
   milliseconds does it cost" with measurements rather than estimates, and then
   stops so the owner can decide.
3. **Phase 2**: gesture recognition, command mapping and OS output on top of the
   phase 1 interfaces, with the gesture-to-command bindings customizable by the
   owner through configuration rather than code edits.
4. Mouse control feature parity with the Python version (cursor movement, left
   click, double click, scroll, and the left-hand arrow-key and print-screen
   bindings) — but with **two hands tracked simultaneously** and a
   **configurable cursor sensitivity**, neither of which the Python version had.
   Both are owner decisions, recorded below.
5. Buildable on both Linux and Windows from a clean checkout with nothing but
   CMake and a compiler.
6. Developable and testable on a machine with **no camera attached**.
7. The known defects in the Python version are not carried across (enumerated
   under Constraints).

## Non-goals

- Draw mode (canvas, header strip, color selection, finger-measured line
  thickness). Deferred to a later phase by the owner's explicit decision. The
  architecture must not make it hard to add, but no part of it is built now.
- Modifying, fixing or optimizing `main.py`. The owner decided to leave the
  Python version untouched. It stays in the repository as-is.
- Camera connection and enumeration beyond a plain device index or file path.
  The owner connects a phone via DroidCam and stated they will handle that
  themselves; the builder should not design for DroidCam specifically, nor build
  camera-discovery UI.
- Any GUI toolkit (Qt, Dear ImGui, GTK). The Python version used `tkinter` for
  camera selection; there is no equivalent that preserves the single-clean-binary
  goal.
- Wayland support in v1 (see Constraints).
- Mobile implementation. Mobile only constrains the *interfaces* now; no mobile
  code is written.

## Anti-goals

These would make the project a failure even if it technically worked:

- **A build that needs Bazel.** MediaPipe's own C++ library was rejected for
  exactly this reason: it would trade the PyInstaller problem for a worse one and
  destroy the clean-native-build goal.
- **OpenCV types in the core.** If `cv::Mat` appears in `HandTracker`,
  `GestureRecognizer` or `ActionMapper` signatures, a future mobile build is
  forced to drag OpenCV along, and the layering is decorative rather than real.
- **Pixel coordinates in the core.** This is the specific thing that makes the
  Python version unreusable: `int(landmark.x * 1280)` is computed at the
  detection boundary and every downstream threshold is implicitly tied to
  1280×720.
- **A core that cannot be tested without a camera.** The development machine has
  no camera. If verifying a gesture change requires plugging in hardware and
  waving a hand, the test suite will not be written.
- **Re-porting the Python bugs** into C++ and calling it a rewrite.

## Constraints

### Development machine (verified 2026-11-12)

- `cmake` 3.28.3, `clang++` 18.1.3, `ninja` 1.11.1 present.
- `g++`, `bazel`, `vcpkg`, `conan` **absent**. OpenCV dev package absent
  (`pkg-config --modversion opencv4` fails). ONNX Runtime absent.
- CPU: Intel i5-12400, 12 threads. GPU: Intel UHD Graphics 730 (integrated) —
  OpenVINO execution provider is a possible later option, not a requirement.
- Session type is **X11**, `DISPLAY=:0`, and `libXtst.so.6` is present. Synthetic
  input injection via XTest therefore works and needs no elevated privileges.
- `/dev/uinput` exists but is `root`-owned and mode `0600`; the user is **not** in
  the `input` group. The uinput path would require privilege changes and is not
  the chosen route.
- **No `/dev/video*` devices.** There is no camera on this machine.

### Platform constraints

- Linux input injection: X11 + XTest. Under Wayland, synthetic input injection is
  blocked by design and XTest does not work; a Wayland build would need
  `libei`/portals. Out of scope for v1, but the `OutputSink` seam is where that
  would later land, and nothing above it should need to change.
- Windows input injection: `SendInput`.
- Mobile (future): system-wide input injection is **impossible** inside the
  application sandbox on both Android and iOS. The mobile shape is therefore
  necessarily: phone captures and recognizes, then sends commands over the
  network to a desktop that injects them. This is why a network `OutputSink` is
  part of the design from the start rather than retrofitted.

### Defects in the Python version that must not be reproduced

These were found while reading `main.py` and are listed so the builder treats
them as requirements, not as behaviour to copy:

1. **Asset load order.** `os.listdir('header')` returns filesystem order; on this
   machine that is `['3.png','4.png','default.png','1.png','2.png']`, so
   `overlayList[0]` is `3.png` and `overlayList[-1]` is `2.png`, not
   `default.png`. Every header-to-color pairing in the draw mode is mismatched.
   Any asset loading must be by explicit name, never by directory order.
   (Relevant only when draw mode is eventually built, but the rule is general.)
2. **`CountFingers` crashes on an empty hand** (`main.py:55`): when
   `len(hand) == 0` the local `fingers` is never bound and the `return` raises
   `UnboundLocalError`. Absence of a hand is a normal state and must be
   representable, not an error path.
3. **`handColor` can be unbound** (`main.py:186`) when handedness is neither
   label.
4. **Stale cross-hand state.** `rightFingers` / `leftFingers` are module globals
   that retain the last frame in which a hand was visible, so gestures keep
   firing after the hand leaves the frame. Gesture state must be explicitly
   invalidated when a hand is lost.
5. **The screenshot latch never holds** (`main.py:270`): the `else` binds to the
   combined condition, so `screenshoted` resets every frame and print-screen can
   repeat-fire. Every discrete (one-shot) gesture needs a latch that actually
   holds until the gesture is released.
6. **A wrong and wasteful color conversion** (`main.py:164`):
   `cv2.cvtColor(frame, COLOR_RGB2BGR)` is applied to a frame that is still BGR,
   because `DetectHandInfo` made its own RGB copy. It swaps red and blue
   incorrectly *and* costs a full-frame pass. The surrounding
   `frame.flags.writeable = False` does nothing, since `cvtColor` copies anyway.
7. **Blocking sleeps inside the frame loop.** `pyautogui`'s default
   `PAUSE = 0.1` means every injected action stalls the loop for 100 ms, and
   scroll is injected every frame while the gesture is held. Output injection
   must never block the pipeline; if it can block, it belongs behind a queue.

## Architecture

Five layers in a pipeline, plus one optional presentation layer. The defining
rule: **each boundary is a plain data structure, not a call into the next
layer.** No layer holds a reference to the type of the layer after it. A layer is
wired to its neighbour by the application shell, not by knowing about it.

```
FrameSource        platform adapter    raw frames in
      ↓  ImageView
HandTracker        portable core       ONNX: palm detection + 21 landmarks
      ↓  HandObservation
GestureRecognizer  portable core       pure logic, no OS, no OpenCV
      ↓  GestureEvent
ActionMapper       portable core       config-driven gesture → command
      ↓  Command
OutputSink         platform adapter    XTest / SendInput / Null / network

Overlay            optional            the only layer allowed to use OpenCV UI
```

### Layer 1 — `FrameSource` (platform adapter)

Produces frames. Backends: camera by device index (V4L2 on Linux, DirectShow on
Windows), video file, and a synthetic generator. The synthetic and file backends
are not conveniences — they are how this project is developed and tested at all,
given there is no camera on the development machine.

The camera backend must set MJPEG fourcc and a capture buffer size of 1. On the
Python side these were missing, which is why 1280×720 fell back to YUY2 and
capped throughput; this is the single largest real FPS lever in the whole
project.

Capture runs on its own thread and always hands out the **latest** frame,
dropping stale ones, so that capture and inference overlap instead of serializing.

### Layer 2 — `HandTracker` (portable core)

Frames in, hand observations out, via ONNX Runtime. Two models, mirroring
MediaPipe's own structure:

- a **palm detector**, run only when no hand is currently being tracked
- a **hand landmark** model, run on a cropped and rotated region of interest

When the previous frame produced a confident hand, the ROI for this frame is
derived from the previous landmarks and the palm detector is skipped. This is
both the accuracy mechanism and the performance mechanism; running the landmark
model on the full frame instead would be substantially worse.

**Two hands are tracked simultaneously** (owner decision). This has consequences
the builder must plan for, and they are the main cost centre of phase 1:

- The landmark model runs **once per tracked hand**, so steady-state inference
  cost roughly doubles. This is the dominant factor in whether the phase 1
  measurements come out acceptable.
- Each hand needs its **own** ROI tracking state. The palm detector is skipped
  only for hands that are currently tracked; if one hand is lost while the other
  is held, detection must run for the missing one without disturbing the other.
- Hands must be **associated with the same identity across frames**, and not
  swapped when they come close together or cross. The landmark model's own
  handedness output is a signal but not sufficient on its own — it is computed
  per-crop and can flip. Spatial continuity between frames should be the primary
  association, with handedness as a cross-check.
- The palm detector can return the same physical hand twice, or return a second
  hand that is actually the already-tracked one. De-duplication against
  currently-tracked ROIs is required.

The palm detector's output requires SSD-style anchor decoding and
non-maximum suppression written by hand — MediaPipe did this inside its graph,
and that code does not come with the model file. This is real work and should be
planned for.

Inference runs on a downscaled copy of the frame, not the full 1280×720 one. The
model input is small regardless; the full-resolution color conversion in the
Python version was pure overhead.

### Layer 3 — `GestureRecognizer` (portable core)

Landmarks in, gesture events out. Pure: no OS calls, no I/O, no OpenCV, no
clock it does not receive as an argument. This is the layer that must be fully
unit-testable from recorded or synthetic landmark sequences, with no camera.

It owns the per-finger extension determination, the derived quantities a gesture
might need (pinch distance, palm center, palm normal), and all the temporal
state: latches for one-shot gestures, hysteresis so a gesture does not chatter at
its threshold, and explicit invalidation when a hand is lost. Defects 2, 4 and 5
above are all failures of this layer in the Python version, and all three are
state-machine problems rather than geometry problems.

Because two hands are tracked, gesture state is **per hand** and fully
independent: each hand has its own latches, and losing one hand invalidates only
that hand's state. This is what makes the owner's chosen behaviour possible —
holding an arrow key with the left hand while moving the cursor with the right.
The Python version could not do this, and defect 4 is precisely the shared
global state that prevented it.

### Layer 4 — `ActionMapper` (portable core)

Gesture events in, abstract commands out — `MoveCursorRelative`, `Click`,
`DoubleClick`, `Scroll`, `KeyPress`. Driven by **configuration**, not a hardcoded
`if` chain. This is the layer the owner specifically asked to be customizable:
rebinding which gesture does what must not require editing or recompiling the
recognizer.

Commands are abstract: nothing here names a platform API.

**Cursor movement is relative with a configurable sensitivity multiplier and
smoothing** (owner decision). The Python version mapped one pixel of frame
displacement to one pixel of screen displacement with no multiplier, which means
on a screen wider than the capture frame the cursor cannot be driven across the
display in one motion. The multiplier and the smoothing strength both live in
configuration alongside the gesture bindings.

Smoothing belongs here rather than in `GestureRecognizer`: the recognizer
reports what the hand is doing, and how aggressively to filter that for cursor
comfort is a binding concern. A builder who finds that landmark jitter needs
filtering *before* gesture thresholds as well should filter in both places for
different reasons, not move this one.

### Layer 5 — `OutputSink` (platform adapter)

Commands in, platform effects out. Backends: XTest (Linux/X11), `SendInput`
(Windows), a null sink for tests, a logging sink for debugging, and later a
network sink. Because injection can block, this layer must not stall the
pipeline — defect 7 is exactly this failure.

### Optional — `Overlay`

The preview window and, later, the draw-mode canvas and header strip. The only
layer permitted to use OpenCV's UI and drawing. The pipeline must run fully
headless with this layer absent.

## Decisions already made

### Made by the owner

- **Do not fix or optimize `main.py`.** Rebuild from scratch in C++, in a new
  folder inside the existing repository. The owner was shown the alternative
  (fix the Python first, measure, then decide) and chose to rebuild.
- **ONNX Runtime with MediaPipe's models converted to ONNX.** Chosen over
  MediaPipe's own C++ library (rejected: requires Bazel) and over OpenCV's DNN
  module (rejected: slower, and gaps in operator support).
- **Support Linux and Windows both**, behind a platform branch rather than
  two codebases.
- **Mouse control first; draw mode later.**
- **Phase 1 stops at `HandTracker`.** Gesture and command layers are built on
  top only after the tracker is proven and measured.
- **Layered, with each layer customizable**, and a core reusable by other
  systems later; mobile is the concrete future case to design the interfaces
  against.
- **Dependencies are downloaded by CMake itself.** A clean `git clone` plus
  `cmake` must be enough; no system packages, no `vcpkg`, no `conan`.
- **The owner handles the camera connection** (phone via DroidCam) and asked
  that it not be designed for.
- **Two hands tracked simultaneously**, chosen over the Python version's single
  hand and over a one-hand-now-two-later compromise. The owner accepted the
  roughly doubled inference cost in exchange for genuine dual-hand control.
- **Cursor movement is relative with a configurable sensitivity multiplier and
  smoothing**, chosen over the Python version's unscaled 1:1 mapping and over an
  absolute frame-to-screen mapping.
- **Phase 1 reports its measurements and stops there.** If the numbers are poor,
  the builder does not silently begin optimizing and does not unilaterally
  declare the ONNX approach failed. The measurements go to the owner and the
  decision to proceed, optimize or change approach is the owner's. A consequence
  for planning: phase 1's final deliverable is a **measurement report**, and
  phase 2 does not start automatically.

### Made by me, as the designer

These were not put to the owner. They are my calls, and a builder may revisit
any of them with reason:

- **The core speaks normalized `0.0..1.0` coordinates, never pixels.** Conversion
  to pixels happens only in `Overlay`. This is the single most important
  reusability decision in the document, and the specific fix for what makes the
  Python version resolution-locked.
- **OpenCV is confined to `FrameSource` and `Overlay`.** The core sees a plain
  `ImageView { data, width, height, stride, format }` — a non-owning view over
  pixels, with no OpenCV type in its definition.
- **Camera selection is a `--camera` command-line flag**, plus in-window display
  of what was selected. No separate selection window, since no GUI toolkit is
  being added.
- **`FrameSource` includes file and synthetic backends from the start**, because
  the development machine has no camera and the alternative is an untestable
  project.
- **A future mobile `OutputSink` is a network sink**, because in-sandbox system
  input injection is impossible on mobile. Mobile's role is capture plus
  recognition, with a desktop performing injection.
- **C++17**, chosen for Android NDK compatibility rather than C++20, despite
  clang 18 supporting C++20 fully. Worth revisiting if mobile is dropped.
- **Linux input via XTest, not uinput** — verified available, and needs no
  privilege changes.
- **Phase 1 ships a debug overlay and per-stage timing instrumentation** as
  deliverables, not as afterthoughts. Phase 1's whole purpose is to produce
  numbers.
- **Dependency acquisition shape**: ONNX Runtime via CMake fetching the
  **prebuilt official release archive** per platform (building ONNX Runtime from
  source would pull in Python and protobuf and defeat the clean-build goal);
  OpenCV from source with its module list trimmed to only what is used. OpenCV
  from source is a slow first configure, which is the accepted cost of the
  owner's self-contained-build decision. A builder who finds a better route
  satisfying "clone plus cmake" should take it.
- **The first task of phase 1 is validating the ONNX models**, before any
  architecture is built. MediaPipe ships `.tflite`; pre-converted ONNX versions
  of palm detection and hand landmark exist but their quality must be verified.
  If they are unusable, converting them is roughly a day of additional work — and
  if the models do not work, nothing else in the project matters. This risk was
  raised with the owner and accepted.
- **Model files are fetched by CMake at configure time** and not committed to the
  repository, consistent with the owner's "clone plus cmake is enough"
  decision. Their expected checksums are pinned so a silently changed model
  cannot alter behaviour.
- **The Windows path is kept compiling but is not verified** during phase 1 and
  phase 2. There is no Windows machine available, and claiming a tested Windows
  build without having run one would be false. The platform seams are built, the
  Windows backends are written, and verification is an explicit later task.
- **Phase 1's measurement report breaks timing down per stage** (capture wait,
  color convert and resize, palm detection, landmark inference per hand,
  association, total end-to-end latency) rather than reporting a single FPS
  number. A single number would not tell the owner which decision to make, and
  making that decision is the owner's stated role.
- **Repository layout**: the C++ project lives in `cpp/`, building a `hc_core`
  library target (portable layers, no platform or OpenCV dependency) plus a
  `hand_controller` executable (adapters, shell, overlay). The library/executable
  split is what mechanically enforces the anti-goals: if `hc_core` compiles
  without OpenCV and without X11 in its link line, the layering is real.

## Left to the builder

- All naming, file organization within `cpp/`, and header layout.
- The concrete form of the boundary structures, beyond the required properties:
  `HandObservation` carries handedness, 21 normalized landmarks, a confidence
  value and a timestamp; `GestureEvent` and `Command` shapes are the builder's
  call.
- The configuration format for `ActionMapper` bindings, and whether it is
  reloadable at runtime.
- The test framework, and the exact mechanism for recorded landmark fixtures.
- Threading structure beyond the stated requirement that capture is threaded and
  output cannot stall the pipeline.
- Whether to add the OpenVINO execution provider for the integrated GPU, and
  when. Only worth doing if CPU inference measurements justify it.
- The PyInstaller-equivalent packaging step for the native binary. Out of phase 1
  and phase 2 scope, but it is one of the actual justifications for the port, so
  it should not be forgotten.
