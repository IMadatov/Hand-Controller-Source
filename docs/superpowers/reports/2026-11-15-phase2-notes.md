# Phase 2 notes

Date: 2026-11-15
Branch: `hand-controller-cpp-phase2`, 12 commits on top of `cdebee9`
Plan: `docs/superpowers/plans/2026-11-14-hand-controller-cpp-phase2.md`

Phase 2 built the three layers above the phase 1 tracker — gesture
recognition, action mapping, output injection — plus the module split they
landed on. **All twelve of the plan's tasks are addressed, and Linux input
injection works** — though not the way Task 9 said it would: XTest was
abandoned for `/dev/uinput`, which is verified to move the cursor on this
Wayland session. See "Task 9 was replaced, not completed" below.

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
| **9 X11 / XTest sink** | **replaced by uinput, and it works** | `platform/src/uinput_sink.cpp`, `tools/uinput_probe.cpp` |
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

## Task 9 was replaced, not completed

The plan offered two ways out of the Wayland block: switch the session to X11,
or target Wayland through `libei`. **The first turned out to be impossible on
this machine** — Ubuntu 26.04 ships GNOME 50, which dropped the Xorg session
entirely: `/usr/share/xsessions` does not exist and the display manager offers
Wayland only. The plan's "the owner has chosen to switch to X11" was not a
choice anyone could make here.

A third route the plan did not consider turned out to be the right one.
**`/dev/uinput`** asks the KERNEL to create a virtual input device, so the
compositor sees events arrive as if from hardware and routes them like any
mouse. No display protocol is involved, which is why it works on Wayland and
X11 alike — and unlike `libei` it needs nothing linked, since `open`, `ioctl`
and `write` are libc. The project forbids build-time system dependencies, so
that was decisive.

Verified the same way XTest was disproved, and in the same order — probe
first, sink second:

```
$ ./cpp/build/tools/uinput_probe
open(/dev/uinput): ok
UI_DEV_CREATE: ok, device named "hand-controller probe"
found our evdev node: /dev/input/event23
kernel round-trip: EVENT WAS EMITTED (read back from evdev)
RESULT: the kernel accepted and emitted our events.
```

and the owner confirmed the cursor visibly moved. **That second half cannot be
automated on Wayland** — the cursor position is not readable, which is exactly
what made XTest's failure invisible — so the probe performs a large, reversed,
deliberately obvious movement and asks. An honest "look at the screen" beats a
check that cannot fail.

`UinputSink`'s own 16 tests are not that, though: each one reads the virtual
device's **own evdev node** back, so "the kernel accepted and emitted this
event" is measured per case. They SKIP when `/dev/uinput` is inaccessible,
which is the default, and a skipped run means UNVERIFIED rather than passed:

```bash
sudo -u "$USER" -g input ./cpp/build-app/bin/test_uinput_sink   # 16/16 here
```

### Three things this route taught that XTest would not have

**1. Sub-pixel motion has to be accumulated or the cursor never moves at all.**
`REL_X` is an integer; smoothing makes most frames' deltas a fraction of a
pixel; truncating each frame independently discards every fraction. XTest
positioned absolutely and had no such failure mode, so this is a bug the port
*introduced* — and `SubPixelMotionAccumulatesInsteadOfVanishing` is the test
that pins it. Truncation is symmetric rather than floored, too, or every
leftward frame would bias by a pixel and the cursor would crawl.

**2. There is a startup window where injection silently goes nowhere.** The
kernel creates the device immediately, but udev and the compositor discover it
asynchronously, and anything written before libinput opens the node reaches
nobody. 400 ms is paid once in `Create`. This is a silent failure of precisely
the XTest kind, arrived at from the opposite direction.

**3. Destroying the device revokes every reader's buffered events.** A key-up
written microseconds before `UI_DEV_DESTROY` reaches nobody, and draining the
node afterwards reports nothing even though the write happened — which reads
exactly like a stuck key and is not one. The destructor now waits 30 ms
between releasing and destroying, and the test proves it from a CONCURRENT
reader, which is what libinput actually is.

### Remaining limitations, recorded rather than fixed

- **Pointer acceleration applies.** libinput runs relative motion through the
  user's acceleration curve, so pixels travelled are not pixels asked for.
  `--sensitivity` is the knob. Bypassing it would mean an absolute,
  tablet-style device — more complexity for a cosmetic gain.
- **`--sink xtest` stays unwritten**, and is now unlikely ever to be worth
  writing: uinput covers X11 as well.
- **One-time `sudo` is required** for the udev rule granting `/dev/uinput`
  access. That is the machine owner's decision and the program only prints the
  rule; it does not arrange its own privileges.

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

- **Nothing on injection, on Linux.** That was the open item and it is closed:
  `--sink uinput` works, verified. macOS (`CGEvent`) is untouched and Windows
  is written but never compiled.
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
