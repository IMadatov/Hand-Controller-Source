# Phase 1 measurements

Per-stage timing for the C++ hand-tracking pipeline, as of Task 14. This is a
data record. It deliberately draws **no conclusion about whether to proceed to
phase 2 and starts no optimization work** — the spec reserves that decision for
the owner.

Every number below was produced by `hand_controller --headless` with the
per-stage `Metrics` instrumentation built in Task 3.

---

## Machine and software

| | |
|---|---|
| CPU | **AMD Ryzen 7 7800X3D**, 8 hardware threads (`nproc` = 8) |
| OS | Linux 7.0.0-38-generic |
| Compiler | clang 21.1.8, C++17, CMake 4.2.3 + Ninja |
| Build type | `CMAKE_BUILD_TYPE` **empty** — see the caveat below |
| ONNX Runtime | 1.18.1, CPU execution provider |
| Palm model | `palm_detection_mediapipe_2023feb.onnx`, 192x192 input |
| Landmark model | `handpose_estimation_mediapipe_2023feb.onnx`, 224x224 input |
| OpenCV | 4.14.0, built from source, no FFmpeg, no GStreamer |

> **The plan assumed an i5-12400 with 12 threads. That is not this machine.**
> The hardware above is what the runs were actually executed on, and the
> `--threads 4` configuration therefore uses 4 of 8 available threads, not 4
> of 12.

> **`CMAKE_BUILD_TYPE` is empty**, so our own code (`hc_core`, the warp, the
> decode) was compiled with **no `-O` flag**, i.e. `-O0`. ONNX Runtime and
> OpenCV are prebuilt-optimized and unaffected, but every `preprocess` number
> below is an unoptimized-build number. This is recorded, not corrected: the
> owner decides what to do about it.

---

## What was measured, and what it is not

**No camera and no real recording exist on this machine.** The plan's Step 5
asks for a recorded video of two hands; none could be produced. Instead the
clips were synthesized from the committed test fixture
(`cpp/tests/fixtures/hand_rgb.raw`, 640x480, two crossed open palms), panned
horizontally +-40 px on a 90-frame sine so successive frames genuinely differ:

- `hand_one.avi` — 640x480, one hand, 300 frames, MJPG-in-AVI.
- `hand_two.avi` — 1280x480, that hand beside its mirror image (opposite
  handedness), 300 frames.

These are **not a substitute for real footage**, and the gap matters in one
specific, quantified way described under "The palm detector barely ran" below.

---

## Results

Three repeats per configuration; the **median run** is reported. Run-to-run
spread was under 2% on every stage except `palmdetect`, which fired once per
run and is therefore a single sample, not a distribution.

`preprocess` and `landmark` run **once per tracked hand per frame**, so their
`calls` is 2x the frame count in the two-hand configurations. `total` is
per-frame. All times in milliseconds.

### Per-stage breakdown

| Config | Video | `--max-hands` | `--threads` | preprocess (per call) | landmark (per call) | **total** mean | total p50 | total p95 |
|---|---|---|---|---|---|---|---|---|
| **A** | one hand | 1 | 1 | 2.760 | 2.898 | **5.721** | 5.629 | 5.849 |
| **B** | one hand | 1 | 4 | 2.687 | 0.928 | **3.656** | 3.592 | 3.683 |
| **C** | two hands | 2 | 1 | 2.775 | 2.880 | **11.377** | 11.322 | 11.468 |
| **D** | two hands | 2 | 4 | 2.718 | 0.952 | **7.389** | 7.339 | 7.546 |
| **E** | two hands | 1 | 1 | 2.877 | 2.910 | **5.819** | 5.779 | 6.148 |

`palmdetect`: 8.3–9.1 ms at `--threads 1`, 3.8–8.1 ms at `--threads 4`, **one
call per run** (see below). `associate`: ≤0.001 ms, one call per run —
negligible and not broken out further. `capturewait`: 0.000 ms throughout, as
file playback always had a frame ready.

### End-to-end latency and achieved frame rate

Wall-clock, including video decode and loop overhead:

| Config | per frame | frame rate |
|---|---|---|
| A — 1 hand, 1 thread | 5.72 ms | ~175 fps |
| B — 1 hand, 4 threads | 3.66 ms | ~274 fps |
| C — 2 hands, 1 thread | 11.38 ms | ~88 fps |
| D — 2 hands, 4 threads | 7.39 ms | ~135 fps |

---

## What the second hand costs

The spec names this as the dominant open question, so it was measured as a
**controlled comparison**: configs **C** and **E** run the *same* two-hand
video at the same thread count, differing only in `--max-hands`.

| | total mean |
|---|---|
| E — two hands visible, `--max-hands 1` | 5.819 ms |
| C — two hands visible, `--max-hands 2` | 11.377 ms |
| **Cost of the second hand** | **+5.558 ms, i.e. +95.5%** |

**The second hand very nearly doubles per-frame cost.** It does not share work
with the first. The reason is visible in the call counts: `preprocess` and
`landmark` each run once per hand per frame, and only `palmdetect` and
`associate` are per-frame — and both are negligible in this steady state.
Cost scales linearly with the number of tracked hands.

The same ratio holds at 4 threads: 3.656 → 7.389 ms, +102%.

---

## Thread scaling

| Stage | 1 thread | 4 threads | speedup |
|---|---|---|---|
| landmark | 2.898 | 0.928 | **3.12x** |
| preprocess | 2.760 | 2.687 | **1.03x — none** |
| total (1 hand) | 5.721 | 3.656 | 1.56x |
| total (2 hands) | 11.377 | 7.389 | 1.54x |

`--threads` sets ONNX Runtime's intra-op count, so it accelerates inference
only. `preprocess` is our own hand-written rotated-crop warp (Task 4), which is
single-threaded and does not scale.

**Consequence: the share of per-frame cost spent in our own code rather than in
inference rises sharply with thread count.** For the two-hand configuration:

| | preprocess share of total |
|---|---|
| 1 thread (config C) | 5.550 of 11.377 ms = **49%** |
| 4 threads (config D) | 5.436 of 7.389 ms = **74%** |

At 4 threads, roughly three quarters of the per-frame budget is the warp, not
the models. Stated as an observation only; note it is also an `-O0` number
(see the build-type caveat above), so it is not a clean measurement of the
algorithm.

---

## The palm detector barely ran — read this before trusting the totals

In **every** configuration above, `palmdetect` was called **exactly once**, on
the first frame. After that the detect-then-track state machine kept all slots
full, and each hand rode the ROI derived from its own previous-frame landmarks.

This is the tracker working as designed, but it means **the totals above are a
best-case steady state.** The fixture-derived clips have no occlusion, no hand
leaving the frame, and no presence drop, so re-detection never fired.

The cost of being wrong about this is quantified: a palm detection is
**8.3–9.1 ms at 1 thread**, which is larger than an entire one-hand frame
(5.72 ms). `HandTrackerOnnx` re-runs the detector on **every frame in which any
slot is empty** — so a real scene showing one hand with `--max-hands 2` pays
that cost *continuously*, not once.

The synthetic source makes this visible. At 1280x720 its moving pattern
produces intermittent false detections, so `palmdetect` fires every frame:

```
preprocess: calls=558 mean=2.359ms p50=2.158ms p95=2.947ms
palmdetect: calls=300 mean=6.805ms p50=6.767ms p95=7.340ms
landmark:   calls=258 mean=2.917ms p50=2.843ms p95=3.145ms
total:      calls=300 mean=13.702ms p50=14.304ms p95=20.077ms
```

Note `total` p95 of 20.1 ms against a p50 of 14.3 ms — the long tail is frames
where detection and landmark both ran. **Real footage will land somewhere
between this and the tables above, and nothing measured here says where.**

---

## Caveats, collected

1. **No real recording.** Clips are a panned still of a fixture image: no
   motion blur, no lighting variation, no articulation, no occlusion. Getting
   real numbers requires a video from the owner's DroidCam setup.
2. **Re-detection never fired** in the fixture clips. See above — this is the
   largest single source of optimism in the totals.
3. **`-O0` for our own code.** `preprocess` especially is not a fair reading.
4. **Not the planned hardware** (Ryzen 7800X3D / 8 threads, not i5-12400 / 12).
5. **`LatestFrameSource` drops frames on file playback.** It is built to
   discard stale frames in favour of the newest, which is right for a camera;
   on a file it means 3–121 of the 300 frames were skipped depending on
   configuration. Per-stage latencies are unaffected (each is measured per
   call), but "frames processed" is not 300 and is not deterministic.
6. **`associate` is ~0 ms here** only because it ran once per run. With
   continuous re-detection it runs per frame; it is still a small cost, but
   not a measured one.

---

## Reproducing

```bash
cmake --build cpp/build-app
./cpp/build-app/bin/hand_controller --video <clip>.avi --headless \
    --max-hands <1|2> --threads <1|4> --report out.md
```

The fixture-derived clips were generated by a throwaway program using
`cpp/tests/fixture_image.hpp` (`LoadRawFixture`, `MirrorX`, `SideBySide`,
`ShiftX`) plus OpenCV's built-in MJPG encoder. It was not committed; MJPG-in-AVI
is mandatory on this machine, as there is no FFmpeg and no GStreamer.

---

## Addendum, 2026-11-15: what layers 3–5 cost (phase 2)

Gesture recognition, action mapping and command dispatch, measured **with no
tracker**, because there is still no camera on this machine: a run of the real
binary sees no hands, and these layers over an empty observation list do
nothing and would measure nothing. The harness feeds them two hands holding
bound gestures on every frame — a right hand pointing (`move_cursor` on
`held`, so a command per frame) and a left thumb-up (an arrow key held down
for the whole run), which is the owner's target behaviour and the state the
recognizer does the most work in.

Committed as `cpp/tools/bench_control.cpp`, so this is repeatable rather than
a number in a document. 200,000 frames, `-O2`-less build settings unchanged
from phase 1 (see caveat 3 above — this is still not a fair reading of our own
code, only an upper bound on it).

| Layers | Per frame | Share of the 11.38 ms tracker (config C) |
|---|---|---|
| 3 — `GestureRecognizer` | 728 ns | 0.007% |
| 3 + 4 — and `ActionMapper` | 799 ns | 0.007% |
| 3 + 4 + 5 — and `NullSink` | 807 ns | 0.007% |
| 3 + 4 + 5 — and `QueuedSink` | 1,336 ns | **0.012%** |

**The expectation held: it is negligible.** The whole gesture-to-injection
chain costs about 1.3 µs against a tracker frame of 11.4 ms. Two readings
worth recording rather than rounding away:

- **The mapper and the null sink are free next to the recognizer.** 728 → 807
  ns. The recognizer scores every gesture for every hand on every frame, and
  with the takeover scan it does that twice while a gesture is active; the
  mapper walks a 13-line binding list and smooths one delta. Neither is worth
  optimising, and the recognizer is the half that would be if either were.
- **The dispatch queue roughly doubles the cost of the layer it protects** —
  +529 ns for a lock, a push and a condition-variable notify. Still 0.012% of
  a frame, and the thing it buys is that a sink which blocks for 100 ms (the
  pyautogui `PAUSE` this design exists to avoid) costs the frame loop nothing
  at all. That trade is not close.

One honest note on the harness: driven flat out, the loop produced ~750,000
commands per second and the queue dropped 19,973 of them because the worker
could not keep up. **That is an artefact of the benchmark, not a prediction.**
A real frame loop runs at ~90 fps, i.e. four orders of magnitude slower, and
`QueuedSink::DroppedCount()` is what would show the assumption to be wrong in
practice.
