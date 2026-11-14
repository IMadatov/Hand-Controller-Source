# Hand Controller C++ — Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **The checkboxes in this file are not maintained — trust `git log`, not the boxes.**

Date: 2026-11-14
Depends on: phase 1, complete at commit `70e897b`.

**Goal:** Build `GestureRecognizer` → `ActionMapper` → `OutputSink` on top of the phase 1 tracker, reaching mouse-control feature parity with `main.py` — cursor movement, left click, double click, scroll, and the left-hand arrow-key and print-screen bindings — but with **two hands tracked simultaneously and fully independent per-hand state**, and with the gesture-to-command bindings customizable through configuration rather than code edits.

**Architecture:** Three new layers, each a plain-data boundary, added to `hc_core` except the sinks. `GestureRecognizer` and `ActionMapper` are portable core: no OS, no OpenCV, no clock they do not receive as an argument. `OutputSink` is a platform adapter behind a queue, because injection can block and must never stall the frame loop. The application shell wires them; no layer holds a reference to the type of the layer after it.

**Tech Stack:** Unchanged from phase 1 — C++17, CMake + Ninja, clang, GoogleTest. **No new fetched dependencies.** The X11 sink reaches `libXtst` by `dlopen` rather than by linking (see Global Constraints).

**Spec:** `docs/superpowers/specs/2026-11-12-hand-controller-cpp-design.md`

---

## Environment: corrections to the spec

The spec's environment survey is stale. These were re-measured on 2026-11-14 and **supersede** it. A task that relies on the spec's numbers will be wrong.

| Spec says | Actually |
|---|---|
| Intel i5-12400, 12 threads | **AMD Ryzen 7 7800X3D, 8 threads** |
| clang++ 18.1.3, cmake 3.28.3 | **clang 21.1.8, cmake 4.2.3** |
| `g++` **absent** | **`g++` 15.2.0 present** (and it is the only compiler here with an ASan runtime — clang's is not installed) |
| Session type is **X11**; XTest therefore works | **Session was Wayland.** XTest was probed empirically: the extension reports present at v2.2 and `XTestFakeMotionEvent` **does not move the cursor**. |

**The owner has chosen to switch the session to X11 for phase 2.** Before Task 9 is written, re-run the probe and confirm the cursor actually moves. Do not build on the assumption.

> **This is the single most dangerous fact in this plan.** XTest fails *silently*: it returns success and does nothing. An `OutputSink` built on it will pass every test that does not check the real cursor position, and will do nothing on the desktop. Task 9 therefore requires a test that reads the pointer back, not one that merely checks the call returned.

Unchanged from phase 1 and still binding: **no camera** (`/dev/video*` absent), **no FFmpeg and no GStreamer** (video fixtures must be MJPG-in-AVI), and `cpp/build-app` holds a 528 MB OpenCV build that must not be reconfigured from scratch.

---

## Global Constraints

Every task's requirements implicitly include this section.

- **C++17.** Chosen for Android NDK compatibility; do not use C++20 features.
- **`GestureRecognizer` and `ActionMapper` are pure.** No OS calls, no I/O, no OpenCV, no clock they do not receive as an argument. Both must be fully unit-testable from synthetic landmark sequences with no camera and no display. If a test for either needs a display, the design is wrong.
- **`hc_core` must link neither OpenCV nor X11.** Enforced at configure time by `hc_assert_no_forbidden_deps`. The sinks live in the app layer.
- **The core speaks normalized `0.0..1.0` coordinates, never pixels.** Phase 1 established that `DrawObservations` is the only place that multiplies by pixel dimensions. **`OutputSink` becomes the second**, since screen coordinates are pixels — and it is the only other one permitted. `ActionMapper` emits relative, normalized motion.
- **A clean `git clone` plus `cmake` must be enough.** No system packages, no `vcpkg`, no `conan`. **Consequence for the X11 sink:** linking `-lXtst` would add a build-time system dependency and break this rule, and would also make the binary fail to start where the library is absent. The X11 sink therefore **`dlopen`s `libXtst.so.6` and resolves symbols at runtime**, reporting itself unavailable if that fails. `dlopen` is libc; nothing is added to the build.
- **Output injection must never block the frame loop.** Spec defect 7. Any sink that can block sits behind a queue on its own thread, and a full queue drops rather than waits.
- **Two hands, fully independent state.** Spec defect 4. Losing one hand invalidates only that hand's state. The owner's target behaviour — holding an arrow key with the left hand while moving the cursor with the right — is the acceptance criterion.
- **Absence of a hand is a normal state, not an error.** Spec defect 2.
- **`Handedness::Unknown` must be handled everywhere.** Spec defect 3 is an unbound variable on exactly this case. Phase 1's tracker can and does emit it, and handedness scores on the committed fixture are as low as 0.575 — barely over the line. Do not assume the label is reliable.
- **Bindings are configuration, not code.** Rebinding a gesture must not require editing or recompiling the recognizer. The owner asked for this specifically.
- **Do not modify `main.py`.** The Python version stays as it is.
- **Out of phase 2 scope:** draw mode, the header strip, the network sink, Wayland/`libei` injection, mobile.

---

## Review Focus

Conditions a reasonable user will hit, each pinned to the task that owns it.

1. **A hand leaves the frame mid-gesture.** State must be invalidated, a held key must be released, and nothing may keep firing. Spec defect 4, and the most likely way this build hurts someone — a stuck modifier key is worse than a missed click. *(Tasks 3, 8)*
2. **A one-shot gesture is held.** It must fire exactly once and not repeat until released. Spec defect 5, where the latch reset every frame. *(Task 3)*
3. **A gesture hovers exactly at its threshold.** Hysteresis must stop it chattering; a click that fires 30 times a second is the failure. *(Task 3)*
4. **Handedness flips mid-session,** which phase 1 measured at 0.575 confidence on the fixture. A left-hand binding must not start firing right-hand commands. *(Tasks 3, 6)*
5. **The sink blocks or the X server stalls.** The frame loop must keep its rate. Spec defect 7. *(Task 8)*
6. **The screen is larger than the frame.** The Python version's 1:1 mapping could not cross a wide display in one motion; relative movement with a sensitivity multiplier is the fix. *(Task 6)*
7. **The application exits while a key or button is held down.** Every press must have a matching release on shutdown, including on the abnormal paths. *(Tasks 7, 8, 11)*

---

## Task 1: Hand features — the derived quantities

Pure geometry over one `HandObservation`. Separated from the recognizer so the arithmetic is tested without any state machine.

**Files:** Create `cpp/core/include/hc/hand_features.hpp`, `cpp/core/src/hand_features.cpp`. Test `cpp/tests/test_hand_features.cpp`.

**Interfaces:**
- Consumes: `HandObservation`.
- Produces: `struct HandFeatures { std::array<bool, 5> extended; float pinch_distance; Point2 palm_center; float palm_size; };` and `HandFeatures ComputeFeatures(const HandObservation& obs);`

`pinch_distance` is normalized by `palm_size`, **not** raw: a raw distance shrinks as the hand moves away from the camera, which is exactly the resolution-locking the rebuild exists to remove.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(HandFeatures, OpenPalmHasAllFingersExtended) { /* fixture-derived open hand -> all five true */ }
TEST(HandFeatures, ClosedFistHasNoneExtended) { }
TEST(HandFeatures, ThumbUsesASeparateAxisFromTheFingers) {
  // The thumb folds across the palm rather than curling into it, so the
  // tip-below-pip test used for the four fingers reports it wrong.
}
TEST(HandFeatures, PinchDistanceIsScaleInvariant) {
  // The same pose at two scales gives the same pinch_distance within a small
  // tolerance. Pins the normalization by palm_size.
}
TEST(HandFeatures, PalmSizeIsNonZeroForADegenerateHand) {
  // All 21 landmarks identical must not divide by zero.
}
```

- [ ] **Step 2: Run and verify they fail** — `ctest --test-dir cpp/build -R "^HandFeatures\." --output-on-failure`. Expect a compile failure. (Note: `-R HandFeatures` unanchored is fine here, but anchor by habit — phase 1 found `-R Cli` silently matching `ScoreClipping`.)
- [ ] **Step 3: Implement.** Build with `-DHC_BUILD_APP=OFF`; this is core and needs no OpenCV.
- [ ] **Step 4: Run and verify they pass.**
- [ ] **Step 5: Commit** — `feat: per-hand derived features for gesture recognition`

---

## Task 2: The gesture vocabulary

**Files:** Create `cpp/core/include/hc/gesture_event.hpp`. Test: folded into Task 3.

**Interfaces:**
- Produces: `enum class GestureKind { None, Pinch, Fist, OpenPalm, PointIndex, TwoFingerScroll, ThumbUp, ... };` and
  `struct GestureEvent { std::uint32_t track_id; Handedness handedness; GestureKind kind; enum class Phase { Began, Held, Ended } phase; Point2 position; Point2 delta; std::int64_t timestamp_us; };`

`Began`/`Held`/`Ended` is the whole point: a one-shot binding fires on `Began`, a continuous one acts on `Held`, and a cleanup binding releases on `Ended`. A design that emits only a level ("fist is true this frame") cannot express the latch and reproduces defect 5.

No step list; this is a header of plain types, landed with Task 3.

---

## Task 3: `GestureRecognizer` — the state machine

The heart of phase 2, and where three of the seven Python defects live.

**Files:** Create `cpp/core/include/hc/gesture_recognizer.hpp`, `cpp/core/src/gesture_recognizer.cpp`, plus Task 2's header. Test `cpp/tests/test_gesture_recognizer.cpp`.

**Interfaces:**
- Consumes: `HandObservation`, `HandFeatures`.
- Produces: `class GestureRecognizer { public: explicit GestureRecognizer(const GestureConfig&); void Process(const std::vector<HandObservation>& hands, std::int64_t timestamp_us, std::vector<GestureEvent>& out); void InvalidateTrack(std::uint32_t track_id); };`

State is held **per `track_id`**, in a map keyed by it, not in any shared field. A track absent from `hands` on a frame is dropped and emits `Ended` for whatever it was holding.

Hysteresis is two thresholds per gesture (enter and exit, enter stricter), held in `GestureConfig`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(GestureRecognizer, OneShotGestureFiresExactlyOnceWhileHeld) {
  // Feed the same pinch pose for 30 frames: exactly one Began, 29 Held, no
  // second Began. Spec defect 5 -- the Python latch reset every frame.
}
TEST(GestureRecognizer, ReleasingAndRepeatingFiresAgain) {
  // Pinch, release, pinch -> two Began events. The latch must hold, not jam.
}
TEST(GestureRecognizer, LosingAHandEmitsEndedAndClearsState) {
  // Pinch held, then the hand vanishes from `hands`: an Ended is emitted and
  // no further events. Spec defect 4 -- stale globals kept firing.
}
TEST(GestureRecognizer, PerHandStateIsIndependent) {
  // Left fist held while the right hand pinches and releases twice. The left
  // hand's Held stream is uninterrupted. This is the owner's target
  // behaviour and the reason two hands were chosen.
}
TEST(GestureRecognizer, HysteresisStopsChatterAtTheThreshold) {
  // Oscillate the pinch distance by a hair around the enter threshold for 100
  // frames -> one Began, zero extra. Without hysteresis this emits ~50.
}
TEST(GestureRecognizer, EmptyHandVectorIsNormalAndEmitsNothing) {
  // Spec defect 2: absence of a hand is a state, not an error.
}
TEST(GestureRecognizer, UnknownHandednessIsNotTreatedAsLeftOrRight) {
  // Spec defect 3. Phase 1 measured handedness confidence at 0.575 on the
  // fixture, so this is a live case, not a theoretical one.
}
TEST(GestureRecognizer, HandednessFlipMidTrackDoesNotRebindTheGesture) {
  // Review focus 4: the label flips per-crop. Identity follows track_id.
}
TEST(GestureRecognizer, TimestampsArePassedThroughNotRead) {
  // The recognizer never calls the clock; feeding non-monotonic timestamps
  // must not hang or misbehave.
}
```

- [ ] **Step 2: Run and verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run and verify they pass.** Then re-run under ASan — see `hand-controller-cpp-asan-recipe`: use **g++**, not clang.
- [ ] **Step 5: Commit** — `feat: per-hand gesture state machine with latches and hysteresis`

---

## Task 4: Abstract commands

**Files:** Create `cpp/core/include/hc/command.hpp`.

**Interfaces:**
- Produces: `struct Command { enum class Kind { MoveCursorRelative, Click, DoubleClick, Scroll, KeyPress, KeyRelease } kind; Point2 delta; int button; int key; };`

Nothing here names a platform API. `key` is an abstract keysym-like enum, not an X11 `KeySym` and not a Windows `VK_`; each sink translates. A `Command` carrying an X11 type would make the mapper unportable and is the mistake to avoid.

Landed with Task 6.

---

## Task 5: Binding configuration

The layer the owner specifically asked to be customizable.

**Files:** Create `cpp/core/include/hc/bindings.hpp`, `cpp/core/src/bindings.cpp`. Test `cpp/tests/test_bindings.cpp`. Create `cpp/config/default_bindings.txt`.

**Interfaces:**
- Produces: `struct Binding { Handedness hand; GestureKind gesture; GestureEvent::Phase phase; Command command; };`, `struct BindingSet { std::vector<Binding> bindings; float sensitivity; float smoothing; };`, and `bool ParseBindings(const std::string& text, BindingSet& out, std::string& error);`

**Format is a flat line-oriented text file, not JSON or YAML** — a parser for either is a fetched dependency or hand-rolled, and the constraint forbids the first while the second is more code than the whole mapper. A line is `<hand> <gesture> <phase> -> <command> [arg]`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Bindings, ParsesTheDefaultFileShipped) { /* the committed default parses clean */ }
TEST(Bindings, RejectsUnknownGestureNaming It) { }
TEST(Bindings, RejectsUnknownCommandNamingIt) { }
TEST(Bindings, ReportsTheLineNumberOnError) {
  // A config error the user cannot locate is a config format that failed.
}
TEST(Bindings, IgnoresCommentsAndBlankLines) { }
TEST(Bindings, RejectsTwoBindingsForTheSameHandGesturePhase) {
  // Silently letting the last win makes a typo invisible.
}
TEST(Bindings, SensitivityAndSmoothingHaveDefaultsAndAreOverridable) { }
TEST(Bindings, RejectsOutOfRangeSmoothing) { /* must be 0..1 */ }
```

- [ ] **Step 2–5** as before. Commit: `feat: configurable gesture-to-command bindings`

---

## Task 6: `ActionMapper`

**Files:** Create `cpp/core/include/hc/action_mapper.hpp`, `cpp/core/src/action_mapper.cpp`, plus Task 4's header. Test `cpp/tests/test_action_mapper.cpp`.

**Interfaces:**
- Consumes: `GestureEvent`, `BindingSet`.
- Produces: `class ActionMapper { public: explicit ActionMapper(BindingSet); void Map(const std::vector<GestureEvent>& events, std::vector<Command>& out); };`

Cursor movement is **relative, with a sensitivity multiplier and smoothing**, both from config. Smoothing lives here, not in the recognizer: the recognizer reports what the hand did; how hard to filter it for comfort is a binding concern.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ActionMapper, AppliesSensitivityToCursorDelta) { }
TEST(ActionMapper, SmoothingDampensASingleFrameSpike) {
  // Landmark jitter is real; one bad frame must not throw the cursor.
}
TEST(ActionMapper, SmoothingOfZeroIsPassThrough) { /* the identity case */ }
TEST(ActionMapper, LeftAndRightBindingsAreIndependent) {
  // Left arrow-key binding and right cursor binding active together -- the
  // owner's stated target behaviour.
}
TEST(ActionMapper, UnboundGestureProducesNoCommand) { /* not an error */ }
TEST(ActionMapper, EndedPhaseEmitsKeyReleaseForAHeldKeyBinding) {
  // Review focus 1 and 7: the release must be generated, or the key sticks.
}
TEST(ActionMapper, EmitsNoPixelsAnywhere) {
  // Deltas stay normalized. The mapper has no screen dimensions and must not
  // acquire any -- that conversion belongs to the sink.
}
```

- [ ] **Step 2–5.** Commit: `feat: config-driven action mapper with relative cursor and smoothing`

---

## Task 7: `OutputSink` interface, null and logging sinks

**Files:** Create `cpp/core/include/hc/output_sink.hpp` (interface only — it names no platform type, so it is core), `cpp/app/src/null_sink.{hpp,cpp}`, `cpp/app/src/logging_sink.{hpp,cpp}`. Test `cpp/tests/test_output_sink.cpp`.

**Interfaces:**
- Produces: `class IOutputSink { public: virtual ~IOutputSink() = default; virtual void Send(const Command&) = 0; virtual void ReleaseAll() = 0; virtual bool Available() const = 0; };`

`ReleaseAll` exists for review focus 7: shutdown must release every held button and key. `Available()` exists because the X11 sink can legitimately fail to initialise, and the shell needs to say so rather than silently doing nothing — the exact trap the Wayland probe exposed.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(NullSink, SwallowsEverythingAndIsAlwaysAvailable) { }
TEST(LoggingSink, RecordsCommandsInOrder) { }
TEST(LoggingSink, ReleaseAllEmitsAReleaseForEachHeldPressAndNothingForNone) { }
TEST(LoggingSink, DoubleReleaseAllIsIdempotent) { }
```

- [ ] **Step 2–5.** Commit: `feat: output sink interface with null and logging backends`

---

## Task 8: The non-blocking dispatch queue

Spec defect 7, and the defect the spec is most emphatic about.

**Files:** Create `cpp/app/src/queued_sink.{hpp,cpp}`. Test `cpp/tests/test_queued_sink.cpp`.

**Interfaces:**
- Produces: `class QueuedSink : public IOutputSink { public: QueuedSink(std::unique_ptr<IOutputSink> inner, std::size_t capacity); };` — a worker thread drains to `inner`; `Send` never blocks.

**A full queue drops and counts, it does not wait.** Blocking to preserve a backlog of stale cursor deltas is worse than losing them: the Python version stalled the loop 100 ms per action, which is the defect.

Exception: `KeyRelease` and button-release commands **must never be dropped**, or a key sticks down. The queue needs a priority path for releases. This is the subtle part of the task.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(QueuedSink, SendReturnsImmediatelyWhenTheInnerSinkIsSlow) {
  // Inner sink sleeps 100ms per command (the pyautogui PAUSE this exists to
  // avoid). 100 Sends must return in well under one inner call.
}
TEST(QueuedSink, DropsOldestWhenFullRatherThanBlocking) { }
TEST(QueuedSink, NeverDropsAReleaseCommand) {
  // Flood the queue past capacity with moves, then send a KeyRelease: it
  // must still reach the inner sink. A dropped release is a stuck key.
}
TEST(QueuedSink, DestructorDrainsAndReleasesAll) { /* review focus 7 */ }
TEST(QueuedSink, CountsDropsForReporting) { }
```

- [ ] **Step 2–4.** Run under **TSan** as well as ASan — this is the second threaded component in the project, and phase 1's `LatestFrameSource` set the precedent of a TSan pass.
- [ ] **Step 5: Commit** — `feat: non-blocking queued output sink with undroppable releases`

---

## Task 9: The X11 / XTest sink

**Do not start this task until the session is X11 and the probe passes.**

**Files:** Create `cpp/app/src/xtest_sink.{hpp,cpp}`. Test `cpp/tests/test_xtest_sink.cpp`.

**Interfaces:** `class XTestSink : public IOutputSink` plus `static std::unique_ptr<XTestSink> Create();` returning nullptr when unavailable.

`libXtst.so.6` and `libX11.so.6` are **`dlopen`ed**, not linked (see Global Constraints). Resolve `XTestFakeMotionEvent`, `XTestFakeButtonEvent`, `XTestFakeKeyEvent`, `XOpenDisplay`, `XFlush`, `XQueryPointer`, `XCloseDisplay`.

This sink converts normalized deltas to pixels. It is the **second and last** place in the project allowed to.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(XTestSink, CreateReturnsNullWhenNoDisplay) {
  // unsetenv("DISPLAY") -> nullptr, not a crash and not a half-built sink.
}
TEST(XTestSink, MoveActuallyMovesTheRealPointer) {
  // SKIP if Create() returns null. Read the pointer with XQueryPointer,
  // send a relative move, read it back, assert it moved, then restore it.
  //
  // THIS IS THE TEST THAT MATTERS. XTest reports success and does nothing
  // under Wayland -- verified on 2026-11-14. A test that only checks the
  // call returned would have passed on a session where injection is dead.
}
TEST(XTestSink, ClampsToScreenBoundsRatherThanWrapping) { }
TEST(XTestSink, ReleaseAllReleasesAHeldButton) {
  // Query the pointer mask to confirm, not the sink's own bookkeeping.
}
```

These are the first tests in the project that touch the live desktop. Guard every one on `Create()` succeeding so the suite still passes headlessly, and **restore the pointer position afterwards** — a test suite that leaves the cursor somewhere random is a bad citizen.

- [ ] **Step 2–5.** Commit: `feat: X11 XTest output sink via runtime dlopen`

---

## Task 10: The Windows `SendInput` sink

**Files:** Create `cpp/app/src/sendinput_sink.{hpp,cpp}`, guarded by `#ifdef _WIN32`.

Written, compiled on Linux only as a syntax check behind the guard (i.e. not compiled at all), and **never run**. Phase 1's README already states that Windows is unverified; this task does not change that claim, and the README must keep saying so. Do not write a test that pretends to cover it.

- [ ] **Step 1: Implement.** - [ ] **Step 2: Commit** — `feat: Windows SendInput sink, unverified`

---

## Task 11: Wire it into the application shell

**Files:** Modify `cpp/app/src/main.cpp`, `cpp/app/src/cli.{hpp,cpp}`. Test: extend `cpp/tests/test_cli.cpp`.

New flags: `--bindings PATH`, `--sink <xtest|log|null>`, `--sensitivity F`, `--smoothing F`, `--no-inject` (force the null sink; the safe default while developing).

**`--no-inject` should be the default until the owner says otherwise.** A program that grabs the cursor the moment it starts, on a machine where the developer is also working, is hostile.

- [ ] **Step 1: Write the failing CLI tests** — unknown sink name rejected naming it; `--sensitivity` rejecting non-numeric and non-positive; `--bindings` on a missing file failing with the path in the message; `--no-inject` overriding `--sink xtest`.
- [ ] **Step 2–4.** Remember `-R "^Cli\."`, anchored.
- [ ] **Step 5:** Shutdown must call `ReleaseAll` on **every** exit path, including the overlay-quit path and the frame-budget path.
- [ ] **Step 6: Commit** — `feat: wire gesture, mapping and injection into the app shell`

---

## Task 12: Overlay gesture HUD, docs, and the phase 2 report

**Files:** Modify `cpp/app/src/overlay.{hpp,cpp}`, `cpp/README.md`. Create `docs/superpowers/reports/<date>-phase2-notes.md`.

- [ ] **Step 1:** Extend the overlay to draw each hand's current gesture and phase next to its skeleton, colour-keyed by the same `track_id` palette. Keep the pixel conversion inside `DrawObservations`.
- [ ] **Step 2:** Add a test that the HUD text for two hands names both gestures.
- [ ] **Step 3:** Update `cpp/README.md`: the new flags, the bindings file format, **the Wayland limitation stated plainly**, and the `--no-inject` default.
- [ ] **Step 4:** Measure the added per-frame cost of layers 3–5 and append to the phase 1 report's table. The expectation is that it is negligible against the ~11 ms tracker cost — but the spec's whole method is to measure rather than assume, and if it is not negligible that is worth knowing.
- [ ] **Step 5: Commit** — `feat: gesture HUD, phase 2 docs and timing notes`

---

## Notes on decisions this plan makes

- **The X11 sink `dlopen`s rather than links.** It honours the no-system-packages constraint, lets one binary run on a machine with no X11 at all, and turns "injection unavailable" into a value the shell can report instead of a link error or a silent no-op. Given that silent no-op is exactly what Wayland produces, this is the defensive choice.
- **The bindings file is flat text, not JSON.** JSON means a fetched dependency (forbidden) or a hand-rolled parser (more code than the layer it configures). The format needs to express a few hundred bytes of key-value pairs.
- **`GestureEvent` carries Began/Held/Ended rather than a boolean level.** The latch and the release are both impossible to express otherwise, and they are defects 5 and 7 respectively.
- **Releases are undroppable in the queue.** Everything else is a latency/throughput trade; a dropped release is a stuck key, which is a user-visible fault that outlasts the program.
- **`--no-inject` defaults on.** Development happens on the same desktop the program would seize.
- **Smoothing is in the mapper, not the recognizer** — carried over from the spec's own reasoning, recorded here so a later builder does not "tidy" it into the wrong layer.
