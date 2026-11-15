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

### Q3 — still Wayland, but injection now WORKS (updated 2026-11-15, later)

The GUI session's reading was right about XTest, and the probe still prints
`RESULT: XTest SILENTLY DID NOTHING`. But the conclusion has changed, so read
this rather than the earlier answer:

**Injection works on Linux now, through `/dev/uinput`.** X11 turned out to be
unreachable on this machine at all (Ubuntu 26.04 / GNOME 50 ships no Xorg
session), so instead of a display protocol the sink asks the KERNEL to create
a virtual input device. The compositor routes its events like any mouse. It
works on Wayland and X11 alike, and the cursor was observed to move.

Three consequences for the GUI, and the first two are new:

1. **A permission state exists, and it is the common failure.** `/dev/uinput`
   is root-only by default, so the sink refuses to start until a one-time udev
   rule is applied. The GUI must render this as its own distinguishable state
   with the fix shown — not as "engine broken". The engine returns the exact
   shell commands in the error string; show them verbatim.
2. **There is a ~400 ms startup wait** before injection is live, because the
   compositor has not discovered the device before then. If Start() appears
   instant and the first gestures do nothing, that is this window, not a bug.
3. **Unchanged, and still the important one:** "engine running" must not be
   rendered from the fact that commands were dispatched. `SinkAvailable()`
   answers whether the sink can deliver, and a UI showing a green light while
   events are discarded is worse than one showing nothing. See §2 point 4 on
   why available ≠ injecting.

---

## 4. The C ABI has landed — `FfiEngine` can be written now

**Phase 3: the shim is written, tested and building.** The GUI is no longer
blocked on anything from this side.

```
header:  cpp/ffi/include/hc/hand_controller.h      (pure C, heavily commented)
library: cpp/build-app/ffi/libhand_controller.so    (14 MB, 25 exported symbols)
build:   cmake --build cpp/build-app --target hc_ffi
tests:   cpp/tests/test_ffi.cpp                    (25 cases, through the C API only)
```

Read the header rather than this section for the details — it is the contract
and it explains every decision in place. What follows is the orientation.

### What the API gives you

```c
hc_config config; hc_config_defaults(&config);   /* ALWAYS call this first */
config.source = HC_SOURCE_CAMERA;  config.camera_index = 0;
config.sink   = HC_SINK_NONE;      /* preview mode; HC_SINK_UINPUT injects */

char error[1024];
hc_session* s = hc_session_create(&config, error, sizeof error);  /* NULL on failure */
hc_session_start(s);

hc_snapshot snap = { .hands = handbuf, .hand_capacity = 4, ... };
hc_session_poll(s, &snap);                       /* latest frame, never blocks */
hc_session_copy_frame(s, rgba, capacity, &w, &h);/* RGBA, tightly packed */

hc_session_set_paused(s, 1);
hc_session_set_sink(s, HC_SINK_UINPUT, error, sizeof error);
hc_session_bindings_text(s, out, capacity);      /* serialized by C++ */
hc_session_set_bindings_text(s, text, error, sizeof error);
hc_enumerate_cameras(cameras, capacity);
hc_session_stop(s); hc_session_destroy(s);
```

Each of the Dart interface's methods has a direct counterpart:
`start` → `hc_session_create` + `hc_session_start`; `setPaused` →
`hc_session_set_paused`; `setInjectInput` → `hc_session_set_sink`;
`loadBindings`/`saveBindings` → the two bindings-text calls;
`enumerateCameras` → `hc_enumerate_cameras`; `frames` → poll on a timer.

### Six things that will bite if they are not known

1. **THE FRAME LOOP IS ALREADY ON A THREAD, inside the library.** `Step()`
   costs ~11 ms and no UI isolate can pay that 90 times a second, so the
   worker runs in C++ and `hc_session_poll` hands back the latest published
   snapshot. **Do not call poll from a background isolate to "avoid
   blocking"** — it is a memcpy, it is cheap, and an isolate would add a
   copy and a message hop for nothing. Poll from a Ticker or a ~60 Hz timer.
2. **Poll is NOT a queue.** It reports the last stepped frame; polling twice
   without a new frame returns the same `frame_index`. Use that field to
   decide whether to rebuild, or the UI will re-render identical data.
3. **The pixels are a separate call, on purpose.** They are three orders of
   magnitude larger than the landmarks. `hc_session_copy_frame` with a null
   buffer reports the size needed (`width * height * 4`); call it only as
   often as the preview actually repaints.
4. **The caller owns every buffer, and truncation is visible.** Pass
   capacities, read back counts. A count equal to its capacity means there
   may have been more. Nothing here allocates on your behalf, and the only
   thing to free is the session handle.
5. **A failed `set_sink` keeps the previous sink.** Asking to inject is not
   succeeding: `/dev/uinput` may be inaccessible, and the error string
   carries the udev rule to fix it. Show it verbatim and leave the toggle
   where the engine actually is, which `hc_session_sink_available` answers.
6. **`hc_abi_version()` must equal `HC_ABI_VERSION` from the header you
   generated bindings against.** Check it once at startup. A mismatched `.so`
   is otherwise silent until a struct field lands in the wrong place.

### One wart, recorded rather than hidden

The library links GTK, because the platform layer it draws the camera frame
source from also contains the debug overlay, and that needs OpenCV's highgui.
Flutter on Linux is GTK3 too, so the same soname is already in the process and
nothing should conflict — but it is weight the GUI does not use. Splitting the
platform layer in two is the fix and it is not done.

### The `Controller` shape underneath, for context

```cpp
Controller(source, tracker, sink /* may be null */, ControllerConfig, Metrics*)

bool Start();                  // false if already running or already stopped
bool Step();                   // one frame; false when the session is over
void Stop();                   // releases every held key, then closes

void PollObservations(std::vector<HandObservation>&) const;
void PollCommands(std::vector<Command>&) const;
void PollEvents(std::vector<GestureEvent>&) const;
const Frame& LastFrame() const;

void SetBindings(BindingSet);  // mid-session, no restart
bool SinkAvailable() const;
std::uint64_t ProcessedFrames() const;
```

Five properties the C API passes through unchanged, each pinned by a test:

1. **`Poll*` is not a queue.** It reports the LAST stepped frame, and polling
   twice without stepping returns the same thing. A GUI that treats it as a
   drain will show each frame once and then nothing.
2. **Commands are reported whether or not a sink is attached**, and they are
   the same values the sink received. Render "what the gesture did" from
   `PollCommands`, never by instrumenting a sink — in preview mode there
   isn't one.
3. **`PollEvents` is the gesture itself**, including events that matched no
   binding. Live View's "current gesture" belongs here; `PollCommands` is its
   effect, which is a different thing and is often empty.
4. **`SinkAvailable()` is not "injecting".** It answers whether the attached
   sink can deliver. A logging sink is available and moves no cursor. Whether
   a sink injects is the frontend's own knowledge — see §3 Q3 on why blurring
   these two is the Wayland trap.
5. **One instance per session, driven from one thread.** Not thread-safe by
   design, like the frame sources under it. Two sessions means two
   `Controller`s.

And the guarantee that has not changed: a `Controller` with **no sink attached
is a fully supported configuration**, returning `Command`s as plain data. That
is preview mode — show the gesture, move nothing. It is not a hack.

The CLI frontend is the worked example to read:
`cpp/app/src/main.cpp` is now composition, the preview window and the report,
and nothing else.
