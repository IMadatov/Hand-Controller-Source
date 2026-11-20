# Hand Controller — team guide for Claude Code

Hand-tracking desktop controller: MediaPipe palm + landmark models on ONNX Runtime (C++), gestures mapped to cursor/keyboard commands, Flutter desktop GUI over a C ABI.

Read first: `cpp/README.md`, `docs/gui-contract.md`, `docs/superpowers/reports/2026-11-15-phase2-notes.md`.

## The team

The main session is the **lead**. It plans, splits work, delegates, and merges results. It does not write large changes itself.

| Agent | Owns | Use for |
|---|---|---|
| `vision-engineer` | `cpp/base/`, `cpp/vision/`, ORT backend | tracking, landmarks, two-hand association, models |
| `control-engineer` | `cpp/control/`, `cpp/config/` | gestures, bindings, ActionMapper, sinks, Controller |
| `platform-engineer` | `cpp/platform/`, `cpp/ffi/`, `cpp/app/`, `cpp/tools/` | uinput / SendInput, camera, overlay, C ABI |
| `flutter-gui-engineer` | `gui/` | screens, theme, MockEngine / FfiEngine |
| `test-engineer` | `cpp/tests/`, `gui/test/` | run all suites, sanitizers, goldens, failing-test-first |
| `architecture-reviewer` | read-only | review every change before commit |

Definitions are in `.claude/agents/`.

## How the lead runs a task

1. Read the relevant docs and code. Write a short plan: which agents, in what order, what each must deliver.
2. Delegate. Give each agent the exact files, the goal, and the "done" condition. Independent work (e.g. C++ engine + GUI on separate files) runs in parallel.
3. If a change crosses a contract (vocabulary, bindings format, FFI header), the C++ owner goes first, then `flutter-gui-engineer` updates `gui/` in the same task.
4. `test-engineer` runs the affected suites.
5. `architecture-reviewer` reviews the full diff. Fix everything marked CHANGES NEEDED before committing.
6. Report to the owner: what changed, test results, what could not be verified.

## Rules for everyone

- Layering is one-directional and enforced by CMake. Fix the dependency, never the check.
- OpenCV only in `hc_platform`; ORT only in `hc_vision_ort`; `hc_control` is pure.
- Normalized `0..1` coordinates everywhere except `platform/src/overlay.cpp`.
- Every `key_press` needs a `key_release` on `ended`. Nothing may drop a release.
- Never report success when the input sink is not really working.
- Do not modify: `main.py` (the original Python prototype), `docs/superpowers/specs/`, `docs/superpowers/plans/`.
- Do not delete `cpp/build-app` (it holds the OpenCV build).
- No real camera on the build machine: use `--synthetic`, `--video`, or test fixtures, and say what was not measured.

## Commands

```bash
# C++ fast path (no OpenCV)
cmake -S cpp -B cpp/build -G Ninja -DHC_BUILD_APP=OFF && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure

# C++ with app
cmake -S cpp -B cpp/build-app -G Ninja -DHC_BUILD_APP=ON && cmake --build cpp/build-app && ctest --test-dir cpp/build-app --output-on-failure

# Flutter
export PATH="$HOME/development/flutter/bin:$PATH" && cd gui && flutter analyze && flutter test
```
