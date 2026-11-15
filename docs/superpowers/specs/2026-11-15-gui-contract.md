# The GUI contract — what `hc_control` guarantees to a UI

Date: 2026-11-15
Branch: `hand-controller-cpp-phase2`
Companion artifact: `cpp/tools/dump_contract.cpp`
Addressed to: the GUI session (`hand-controller-gui` worktree, `docs/gui-contract.md`)

The GUI is being built in a parallel session against a mock engine. This
document and the tool beside it exist so that the mock is the *real* shape
rather than a transcription of one.

```
cmake --build cpp/build --target dump_contract
./cpp/build/tools/dump_contract > contract.json
```

**The JSON is the normative artifact, not this prose.** It is generated from
`hc_control` — the gesture and command names come from the same functions the
bindings parser uses, the default configuration is the file that ships, and the
sample session is produced by actually running `GestureRecognizer` and
`ActionMapper` over synthetic poses. Where this document and the JSON disagree,
the JSON is right and this document is stale. Re-run the tool after any
`hc_control` change and diff the output: a field that moved shows up there
before it shows up as a broken screen.

---

## 1. What the JSON contains

| Key | What it is |
|---|---|
| `vocabulary` | Every bindable gesture, phase, hand selector, command, mouse button and key, as the exact wire strings |
| `settings` | `sensitivity` and `smoothing` — defaults and valid ranges |
| `recognizer` | Read-only recognizer tuning: per-gesture enter/exit thresholds, `pinch_span`, `steal_confirm_frames` |
| `validation_rules` | The rules `ParseBindings` **enforces**, as prose a UI can show |
| `default_configuration` | The shipped bindings, parsed into structure, plus the file text verbatim |
| `sample_session` | 16 frames of real recognizer and mapper output: hands, landmarks, features, events, commands |

`schema_version` is `1`. It increments when a key changes meaning or
disappears; added keys do not bump it.

`sample_session` is the one to build Live View and Diagnostics against. Every
frame carries the 21 landmarks per hand (frame-normalized `0..1`, MediaPipe
order), the derived features, the gesture events, and the commands the mapper
produced from them. Deltas are **relative and normalized — never pixels**.

---

## 2. One behaviour a UI will get wrong unless it is told

Gestures do not switch on the frame the pose changes.

Five pairs in the vocabulary differ by exactly one finger — `fist`/`point_index`,
`fist`/`thumb_up`, `fist`/`pinky_only`, `open_palm`/`four_fingers`, and
`point_index`/`two_fingers`. The exit threshold deliberately tolerates one
finger of disagreement so a single bad frame of landmarks does not end a
gesture. That same tolerance means a higher-scoring neighbour has to *take
over*, and the takeover is confirmed over `steal_confirm_frames` consecutive
frames (2 by default, ~20 ms).

When it fires, the old gesture emits `ended` on one frame and the new one emits
`began` on a **later** frame. Never the same frame. This is deliberate: a
release and an unrelated press arriving together would let a `key_release` and
a `key_press` be dispatched out of order.

`sample_session` contains the transition, with `point_index` → `fist` at the
end:

| frame | event | command |
|---|---|---|
| 11 | `point_index held` | `move_cursor` |
| 12 | `point_index ended` | — |
| 13 | `fist began` | — (unbound) |
| 14–15 | `fist held` | — |

So a Live View that renders "current gesture" from the event stream has a
one-to-three-frame window where the hand holds nothing. That is the truth, not
a dropped frame — render it as no gesture rather than smoothing it away, or the
UI will claim a state the engine is not in.

`point_index` → `two_fingers` is the same transition and it is the default
configuration's primary interaction: point to move the cursor, raise the middle
finger to click.

---

## 3. Answers to the GUI session's three questions

From `docs/gui-contract.md` §8 on the `hand-controller-gui` branch.

### Q1 — a disabled binding has no representation, and that is correct for now

**Answer: the format does not express it, and the GUI's current behaviour —
drop disabled rows on save, and warn before doing it — is the right one until
the owner decides otherwise.**

Why not a comment convention: `# disabled: any pinch began -> click` would make
the serializer's output depend on state the parser throws away, so
`ParseBindings(SerializeBindings(x)) == x` stops holding for anything carrying
a disabled row. That round-trip property is pinned by a test and is the reason
the writer lives in `hc_control` at all.

If the owner wants persistent disabling, the shape it should take is an
explicit grammar token the parser reads back — a leading `off` on the line —
not a comment, and it must land in `ParseBindings`/`SerializeBindings`
together with the two enforcement rules adjusted: a disabled `key_press` must
not require a `key_release`, and a disabled binding must not count toward the
overlap check. **This is not implemented and is not in phase 2 scope as
written.** It is the owner's call, and it is an engine change, not a GUI one.

### Q2 — which Settings fields are the engine's

Only two of them are, and they live in `BindingSet`:

| Field | Owner | Where |
|---|---|---|
| `sensitivity` | **engine** | `BindingSet`, `set sensitivity` in the bindings file |
| `smoothing` | **engine** | `BindingSet`, `set smoothing` in the bindings file |
| camera index, resolution | **engine, but not configuration** | CLI flags today (`--camera`, `--synthetic WxH`); they are frame-source arguments, not persisted settings |
| detection threshold, max hands, threads | **engine, but not configuration** | CLI flags (`--max-hands`, `--threads`); tracker construction, nothing the bindings file carries |
| scroll sensitivity | **engine — but not separately settable** | `scroll` deliberately shares `sensitivity` and the same jitter filter as the cursor, so it is tunable but not independently. A separate scale would be a new `set` key in `BindingSet`, parser included — ask before showing the user two sliders |
| panic hotkey | **neither, yet** | Nothing in `hc_control` reads a hotkey. It would belong to the application shell (Task 11) or the GUI process, and whichever owns it must call `ReleaseAll` |
| theme, window size, which screen was last open | **GUI** | Keep them in the GUI's own store; the engine has no opinion |

The rule behind the table: if a value changes what a gesture *means* or how a
command is *computed*, it is `hc_control`'s and belongs in the bindings file.
If it changes how frames are *acquired*, it is a construction argument. If it
changes how things *look*, it is the GUI's.

So the GUI holding resolution, detection threshold and panic hotkey in its own
store and writing them nowhere is correct today — but it should not present
them as engine settings that persist, because they do not.

### Q3 — the session is still Wayland, and Task 9 is still blocked

Confirmed, and it is measured rather than assumed: the probe is committed as
`cpp/tools/xtest_probe.cpp` and still prints

```
RESULT: XTest SILENTLY DID NOTHING (returned success, no move)
```

`XDG_SESSION_TYPE=wayland`. The plan's assumption that the owner switches to
X11 has not happened. **The GUI session's reading is right: a Task 9 test that
reads the pointer back is supposed to fail here, and that failure is the
finding, not a bug to fix.**

What this means for the GUI: "engine running" must not be rendered from the
fact that commands were dispatched. Sink availability is reported by the sink's
`Create()`, and unavailability is a visible state (`input injection
unavailable`), never swallowed. A UI that shows a green light while XTest
silently discards every event is worse than one that shows nothing.

---

## 4. What the GUI is still blocked on

`Controller` (Task 11) and the `extern "C"` shim over it. Nothing in this
document unblocks live preview, real start/stop, real bindings I/O, or the
metrics panel. The shim is not written until Task 11 lands, because there is
nothing stable to wrap.

Two guarantees the mock should be built against, because they will hold:

- One `Controller` per session. No singleton, no global state.
- A `Controller` with **no sink attached is a fully supported configuration**.
  It returns `Command`s as plain data. That is the GUI's preview mode: show
  the gesture, move nothing. It is not a hack.
