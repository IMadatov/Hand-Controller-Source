# Phase 2 notes

Date: 2026-11-15
Branch: `hand-controller-cpp-phase2`, 12 commits on top of `cdebee9`
Plan: `docs/superpowers/plans/2026-11-14-hand-controller-cpp-phase2.md`

Phase 2 built the three layers above the phase 1 tracker — gesture
recognition, action mapping, output injection — plus the module split they
landed on. **Eleven of the plan's twelve tasks are done. Task 9 (the X11 sink)
is blocked and should stay blocked**; see below.

263 tests pass in the core-only configure, 288 with the application. ASan
clean (with the documented ORT-leak split), TSan clean on both threaded
components.

One of those twelve is written and cannot be verified from here at all: the
Windows `SendInput` sink sits entirely behind `#ifdef _WIN32`, so on this
machine **not even the compiler has read it**. It is reachable through
`--sink sendinput`, which refuses on non-Windows, and it deliberately has no
test -- a test that cannot run would assert nothing while looking like
coverage.

---

## What landed

| Task | Status | Where |
|---|---|---|
| 1 Hand features | done | `vision/src/hand_features.cpp` |
| 2 Gesture vocabulary | done | `control/include/hc/gesture_event.hpp` |
| 3 `GestureRecognizer` | done | `control/src/gesture_recognizer.cpp` |
| 4 Abstract commands | done | `control/include/hc/command.hpp` |
| 5 Binding configuration | done | `control/src/bindings.cpp`, `config/default_bindings.txt` |
| 6 `ActionMapper` | done | `control/src/action_mapper.cpp` |
| 7 Sink interface, null + logging | done | `control/` — not `platform/`, see below |
| 8 Dispatch queue | done | `control/src/queued_sink.cpp` |
| **9 X11 / XTest sink** | **BLOCKED** | nothing written; `tools/xtest_probe.cpp` is why |
| 10 Windows `SendInput` | written, **never compiled** | `platform/src/sendinput_sink.*` |
| 11 Application shell | done, as `Controller` | `control/src/controller.cpp`, `app/src/main.cpp` |
| 12 HUD, docs, measurements | done | `platform/src/overlay.cpp`, `README.md`, this report |

Beyond the plan: `tools/dump_contract.cpp` and
`specs/2026-11-15-gui-contract.md`, for the parallel Flutter GUI session, and
`tools/bench_control.cpp` for the Task 12 measurement.

---

## Four places this diverged from the plan, and why

**1. The null, logging and queued sinks live in `hc_control`, not
`platform/`.** None of the three contains a platform API — two are a dozen
lines of bookkeeping and the third is a `std::thread` with a mutex. Filing
them under `platform/` would have chained the `--no-inject` default path, and
every headless, server and browser configuration, to OpenCV. Only Task 9's and
Task 10's sinks are genuinely per-system.

**2. Task 11 extracted a `Controller` instead of writing the loop into
`main()`** — the plan's own amendment, carried out. `main.cpp` is now
composition, the preview window and the report; a GUI, a .NET host or a server
drives the same loop rather than reimplementing it.

**3. A confirmed takeover was added to the recognizer, which the plan did not
anticipate.** Found by running the real recognizer over a synthetic session
rather than by reading the code: the exit threshold tolerates one finger of
flicker, five pairs in the vocabulary differ by exactly one finger, and the
combination made the transition between them impossible. `point_index` held
through a `two_fingers` pose forever — so the shipped configuration's click,
the primary interaction, could never fire. A higher-scoring gesture now takes
over after `steal_confirm_frames` consecutive winning frames (2, ~20 ms),
ending the old gesture on one frame and beginning the new one on a later one.

**4. `QueuedSink`'s undroppable rule had to be enforced on eviction, not only
on insertion.** "Drop the oldest when full" written as `pop_front()` evicts a
queued `KeyRelease` on the next burst of cursor motion, and the plan's own
`NeverDropsAReleaseCommand` test does not catch it because it sends the
release last. It surfaced in a concurrent-senders test that lost exactly one
release out of four.

---

## Task 9 is blocked, and the block is a finding

`XDG_SESSION_TYPE=wayland`. XTest reports its extension present at version
2.2, `libXtst.so.6` is installed, `XTestFakeMotionEvent` returns success — and
the cursor does not move. Every signal short of reading the pointer back says
it works.

The probe is committed (`cpp/tools/xtest_probe.cpp`), so this is repeatable
rather than a claim. It still prints `RESULT: XTest SILENTLY DID NOTHING` as
of this date.

`--sink xtest` is therefore accepted by the parser and **refused at
construction**, naming the probe and the reason. Shipping a sink that returns
success while moving nothing is the precise failure this project exists to
avoid, and a half-written one would have passed every test that did not read
the real pointer.

Two ways forward, and the choice is the owner's: log into an X11 session and
re-run the probe until it prints `ACTUALLY MOVES`, or target Wayland properly
through `libei` — note the running XWayland already carries
`-enable-ei-portal`. The second is a larger piece of work than Task 9 and was
explicitly out of phase 2 scope.

---

## Measurements

Layers 3–5, two hands holding bound gestures, 200,000 frames
(`cpp/tools/bench_control`):

| Layers | Per frame | Share of the 11.38 ms tracker frame |
|---|---|---|
| `GestureRecognizer` | 728 ns | 0.007% |
| + `ActionMapper` | 799 ns | 0.007% |
| + `NullSink` | 807 ns | 0.007% |
| + `QueuedSink` | 1,336 ns | **0.012%** |

Negligible, as expected — but measured, which is the point. The full table and
its caveats are appended to
`docs/superpowers/reports/2026-11-12-phase1-measurements.md`.

What this says about where the time goes: the whole gesture-to-injection chain
is four orders of magnitude cheaper than the tracker frame it rides on. **If
phase 3 wants a faster frame, there is nothing here worth optimising** — the
palm detector re-running while any tracking slot is empty is still the cost
that matters, exactly as phase 1 found.

---

## What phase 3 inherits

- **Task 9**, when there is an X11 session or a `libei` decision.
- **The `extern "C"` shim over `Controller`**, which is what would unlock .NET
  P/Invoke, Flutter FFI and mobile from one artifact. Deliberately not written
  before now, because there was nothing stable to wrap; `Controller` is that
  thing, and the GUI session has the shape (`specs/2026-11-15-gui-contract.md`
  §4).
- **Two open format questions** the GUI surfaced, both the owner's call and
  both recorded in that spec: persisting a *disabled* binding, and whether
  scroll wants a sensitivity separate from the cursor's.
- **Nothing calibrated against a real hand.** Every threshold in
  `GestureConfig::Default` and both settings in `default_bindings.txt` are
  CHOSEN, not measured — there is still no camera on this machine. They are
  starting points for tuning, and the first session with a working camera
  should expect to change them.
