#ifndef HC_HAND_CONTROLLER_H
#define HC_HAND_CONTROLLER_H

/* The C ABI over hc::Controller.
 *
 * WHAT THIS IS FOR. One artifact, several hosts: Dart FFI for the Flutter GUI
 * first, and .NET P/Invoke, a mobile build or a plain C client after it with
 * no second implementation of the pipeline. The plan deliberately forbade
 * writing this until Controller existed, because there was nothing stable to
 * wrap; Controller landed in phase 2 and this is the wrapper.
 *
 * THE RULES THIS BOUNDARY KEEPS, each because the alternative is a crash in
 * someone else's language:
 *
 *   1. NO C++ TYPES CROSS. No std::string, no std::vector, no exceptions, no
 *      inheritance. Plain structs with fixed layout and explicit sizes, so a
 *      Dart `struct` declaration can mirror them field for field.
 *
 *   2. NO EXCEPTION ESCAPES. Every entry point catches everything and turns
 *      it into a return code plus a message the caller can read. An exception
 *      unwinding into the Dart runtime terminates the process.
 *
 *   3. THE CALLER OWNS EVERY BUFFER. Nothing here returns memory the caller
 *      must free, except the session handle itself, which has exactly one
 *      destroy function. Counts are passed in as capacities and come back as
 *      how many were written, which makes a too-small buffer a truncation the
 *      caller can see rather than a write past the end.
 *
 *   4. ONE SESSION PER HANDLE, and the handle is not thread-safe. Controller
 *      is not either, by design. Internally this library runs the frame loop
 *      on its own thread -- Step() costs ~11 ms and a UI thread cannot pay
 *      that 90 times a second -- and `hc_session_poll` hands back the latest
 *      published snapshot. So the CALLER never blocks on a frame, which is
 *      the whole reason the thread is in here rather than in Dart.
 *
 * INTEGER ENUM VALUES ARE PART OF THE ABI. They match the C++ enums exactly
 * (hc::Handedness, hc::GestureKind, hc::GestureEvent::Phase,
 * hc::Command::Kind, hc::MouseButton, hc::Key) and a static_assert in the
 * implementation pins each one. Prefer the hc_*_name functions for anything a
 * user reads: they return the same strings the bindings file uses, so a UI
 * cannot drift from the configuration format.
 */

#include <stdint.h>

/* Export marker. Everything in this header carries it and nothing else in the
 * library does: with -fvisibility=hidden that makes the extern "C" surface
 * below the ONLY thing a host can reach.
 *
 * That is not tidiness. Without it the library exports its STATIC
 * DEPENDENCIES' symbols -- 147 of them, from OpenCV and ONNX Runtime -- while
 * hiding the API, so a host holding its own copy of either can bind to ours
 * and crash somewhere unrelated. */
#if defined(_WIN32)
#  if defined(HC_BUILDING_SHARED)
#    define HC_API __declspec(dllexport)
#  else
#    define HC_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define HC_API __attribute__((visibility("default")))
#else
#  define HC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Landmarks per hand. Fixed by the model, not a preference. */
#define HC_LANDMARK_COUNT 21

/* Bumped when a change breaks a caller compiled against an older header.
 * Check it at startup: a mismatched .so is otherwise silent until a field
 * lands in the wrong place. */
#define HC_ABI_VERSION 1
HC_API int32_t hc_abi_version(void);

/* --- enums, values fixed by the ABI ------------------------------------- */

typedef enum {
  HC_HANDEDNESS_UNKNOWN = 0,
  HC_HANDEDNESS_LEFT = 1,
  HC_HANDEDNESS_RIGHT = 2
} hc_handedness;

typedef enum {
  HC_GESTURE_NONE = 0,
  HC_GESTURE_PINCH = 1,
  HC_GESTURE_FIST = 2,
  HC_GESTURE_OPEN_PALM = 3,
  HC_GESTURE_POINT_INDEX = 4,
  HC_GESTURE_TWO_FINGERS = 5,
  HC_GESTURE_THUMB_UP = 6,
  HC_GESTURE_PINKY_ONLY = 7,
  HC_GESTURE_FOUR_FINGERS = 8
} hc_gesture_kind;

typedef enum {
  HC_PHASE_BEGAN = 0,
  HC_PHASE_HELD = 1,
  HC_PHASE_ENDED = 2
} hc_gesture_phase;

typedef enum {
  HC_COMMAND_NONE = 0,
  HC_COMMAND_MOVE_CURSOR = 1,
  HC_COMMAND_CLICK = 2,
  HC_COMMAND_DOUBLE_CLICK = 3,
  HC_COMMAND_SCROLL = 4,
  HC_COMMAND_KEY_PRESS = 5,
  HC_COMMAND_KEY_RELEASE = 6
} hc_command_kind;

typedef enum {
  HC_BUTTON_LEFT = 0,
  HC_BUTTON_RIGHT = 1,
  HC_BUTTON_MIDDLE = 2
} hc_mouse_button;

typedef enum {
  HC_KEY_ARROW_UP = 0,
  HC_KEY_ARROW_DOWN = 1,
  HC_KEY_ARROW_LEFT = 2,
  HC_KEY_ARROW_RIGHT = 3,
  HC_KEY_PRINT_SCREEN = 4,
  HC_KEY_ESCAPE = 5,
  HC_KEY_ENTER = 6,
  HC_KEY_SPACE = 7,
  HC_KEY_TAB = 8
} hc_key;

/* Where frames come from. Exactly one per session. */
typedef enum {
  HC_SOURCE_CAMERA = 0,
  HC_SOURCE_VIDEO = 1,
  /* Generated frames. No camera, no hands -- for a UI that wants to run its
   * own plumbing end to end before a camera exists. */
  HC_SOURCE_SYNTHETIC = 2
} hc_source_kind;

/* Where commands go.
 *
 * HC_SINK_NONE is a SUPPORTED configuration, not a degraded one: commands are
 * still produced and still reported, and nothing is moved. That is preview
 * mode, and the only sane mode for a server. */
typedef enum {
  HC_SINK_NONE = 0,
  HC_SINK_LOG = 1,
  /* Linux. A virtual input device in the kernel, so it works under Wayland
   * and X11 alike. Needs one-time access to /dev/uinput; when it does not
   * have it, creation fails with the udev rule in the error message. */
  HC_SINK_UINPUT = 2
} hc_sink_kind;

/* The same strings the bindings file uses, so a UI and a configuration cannot
 * drift apart. Never NULL: an unknown value returns "none" or equivalent. */
HC_API const char* hc_handedness_name(int32_t handedness);
HC_API const char* hc_gesture_name(int32_t gesture);
HC_API const char* hc_phase_name(int32_t phase);
HC_API const char* hc_command_name(int32_t kind);
HC_API const char* hc_button_name(int32_t button);
HC_API const char* hc_key_name(int32_t key);
HC_API const char* hc_sink_name(int32_t sink);

/* --- per-frame data ----------------------------------------------------- */

/* Normalized, 0..1 across the frame, origin top-left. NEVER pixels: exactly
 * two places in the project multiply by a resolution, and both are below this
 * boundary. A pixel value arriving here means a layer leaked. */
typedef struct {
  float x;
  float y;
} hc_point;

typedef struct {
  uint32_t track_id;
  int32_t handedness;       /* hc_handedness */
  float handedness_score;   /* 0..1, measured as low as 0.575 in practice */
  float presence;           /* 0..1 */
  int64_t timestamp_us;
  hc_point landmarks[HC_LANDMARK_COUNT];
} hc_hand;

typedef struct {
  uint32_t track_id;
  int32_t handedness;  /* LATCHED at `began`, not re-read per frame */
  int32_t gesture;     /* hc_gesture_kind */
  int32_t phase;       /* hc_gesture_phase */
  hc_point position;
  hc_point delta;      /* relative; zero on `began` */
  int64_t timestamp_us;
} hc_event;

typedef struct {
  int32_t kind;    /* hc_command_kind */
  hc_point delta;  /* move_cursor and scroll only */
  int32_t button;  /* click and double_click only */
  int32_t key;     /* key_press and key_release only */

  /* Index into this snapshot's events of the gesture that caused this, or -1.
   * A command cannot explain itself -- this is how a log says which hand and
   * which gesture produced an entry. */
  int32_t origin_event;

  /* 1 when a sink actually received it. 0 in preview mode and while paused,
   * where the command was produced and deliberately not delivered. */
  int32_t delivered;
} hc_command;

/* One stepped frame, and the unit a caller should think in.
 *
 * A FRAME IS THE UNIT, not three independent streams: hands, events and
 * commands all describe the same instant, and pairing this frame's hands with
 * last frame's commands is the mistake this struct exists to prevent.
 *
 * Fill the three pointers and the three capacities before calling
 * hc_session_poll; it writes the three counts. A count equal to its capacity
 * means output may have been truncated. Any pointer may be NULL with its
 * capacity 0, for a caller that wants only the metrics. */
typedef struct {
  hc_hand* hands;
  int32_t hand_capacity;
  int32_t hand_count;

  hc_event* events;
  int32_t event_capacity;
  int32_t event_count;

  hc_command* commands;
  int32_t command_capacity;
  int32_t command_count;

  int64_t timestamp_us;

  /* Monotonic count of stepped frames. THE SAME VALUE TWICE MEANS NO NEW
   * FRAME ARRIVED -- poll is not a queue, it reports the latest snapshot, and
   * a caller polling faster than the pipeline steps will see repeats. */
  uint64_t frame_index;

  /* Frames the capture thread discarded as stale. Surfaced rather than
   * hidden: a smooth-looking 10 fps with half the frames dropped is a
   * different problem from an honest 10 fps. */
  uint64_t frames_dropped;

  double fps;         /* smoothed over recent frames */
  double latency_ms;  /* frame received to commands produced */

  int32_t frame_width;
  int32_t frame_height;
} hc_snapshot;

/* --- session ------------------------------------------------------------ */

typedef struct hc_session hc_session;

typedef struct {
  int32_t source;          /* hc_source_kind */
  int32_t camera_index;    /* HC_SOURCE_CAMERA */
  const char* video_path;  /* HC_SOURCE_VIDEO; borrowed, copied immediately */
  int32_t synthetic_width;
  int32_t synthetic_height;

  /* A CEILING, NOT A TARGET, and the distinction is expensive: the palm
   * detector re-runs on every frame in which any slot is empty, so 2 in a
   * one-hand scene costs a detection (~8 ms) per frame. */
  int32_t max_hands;

  int32_t threads;  /* ONNX Runtime intra-op threads */

  /* The bindings, in the file format. NULL or empty means the configuration
   * compiled into the library. Parsed here, so a UI never implements the
   * format: that is the one rule this boundary exists to enforce. */
  const char* bindings_text;

  /* Overrides for the text's own `set` lines. <= 0 means "leave it alone",
   * which is why 0 is not a usable sensitivity -- the parser rejects it too. */
  float sensitivity;
  float smoothing;

  int32_t sink;  /* hc_sink_kind */

  /* What a normalized delta of 1.0 is multiplied by, for HC_SINK_UINPUT.
   * 0 means detect it from /sys/class/drm. */
  int32_t screen_width;
  int32_t screen_height;
} hc_config;

/* Fills `out` with the defaults: camera 0, two hands, one thread, the
 * compiled-in bindings, no sink. Call this before setting fields, so a field
 * added in a later version does not arrive as whatever was on the stack. */
HC_API void hc_config_defaults(hc_config* out);

/* Builds a session. Returns NULL on failure and writes a human-readable
 * reason to `error` (NUL-terminated, truncated to fit).
 *
 * The models are loaded here, so this is the slow call -- expect tens of
 * milliseconds. The camera is NOT opened yet; hc_session_start does that. */
HC_API hc_session* hc_session_create(const hc_config* config, char* error,
                              int32_t error_capacity);

/* Stops the session if running -- which releases every held key -- and frees
 * it. Safe on NULL. The handle is dead afterwards. */
HC_API void hc_session_destroy(hc_session* session);

/* Opens the frame source and starts the worker thread. Returns 1 on success,
 * 0 on failure with the reason available from hc_session_last_error.
 *
 * A session that has been stopped does not restart: the source is closed and
 * that is terminal. Create another. */
HC_API int32_t hc_session_start(hc_session* session);

/* Stops the worker, releases everything the sink holds, closes the source.
 * Idempotent and safe before start. */
HC_API void hc_session_stop(hc_session* session);

/* 1 while the worker is stepping frames. */
HC_API int32_t hc_session_running(hc_session* session);

/* Copies the latest published snapshot into the caller's buffers. Returns 1
 * if a frame was available, 0 if none has been stepped yet. Never blocks for
 * longer than it takes to copy. */
HC_API int32_t hc_session_poll(hc_session* session, hc_snapshot* snapshot);

/* Copies the latest frame's pixels as RGBA, tightly packed
 * (width * height * 4 bytes), which is what a UI toolkit wants to upload.
 *
 * Returns the number of bytes needed, or 0 if there is no frame. Writes
 * nothing and still returns the needed size when `capacity` is too small, so
 * a caller can size its buffer in one extra call. `width` and `height` may be
 * NULL.
 *
 * SEPARATE FROM poll ON PURPOSE: the pixels are three orders of magnitude
 * larger than the landmarks, and a UI that only needs the gesture should not
 * pay for a frame copy it throws away. */
HC_API int32_t hc_session_copy_frame(hc_session* session, uint8_t* dst,
                              int32_t capacity, int32_t* width,
                              int32_t* height);

/* Keeps the camera and the recognizer running, stops delivering to the sink.
 *
 * Entering pause RELEASES whatever the sink holds: pausing while a gesture
 * holds a key would otherwise leave it down for the pause. Leaving pause
 * presses nothing again. */
HC_API void hc_session_set_paused(hc_session* session, int32_t paused);

HC_API int32_t hc_session_paused(hc_session* session);

/* Swaps the sink while running -- the GUI's "inject input" toggle.
 *
 * Returns 1 on success, 0 with a reason in `error`. A failure leaves the
 * PREVIOUS sink in place, so a refused uinput (no /dev/uinput access) does
 * not silently become preview mode.
 *
 * The outgoing sink's held keys are released first: it is the only thing that
 * can release what it pressed, and a moment later it will not exist. */
HC_API int32_t hc_session_set_sink(hc_session* session, int32_t sink, char* error,
                            int32_t error_capacity);

/* 1 when a sink is attached AND reports itself able to deliver.
 *
 * NOT "is it injecting". A logging sink is available and moves no cursor;
 * which sinks inject is the caller's own knowledge, because the caller chose
 * one. Conflating these is the Wayland trap: XTest returns success and does
 * nothing, so a UI that showed a green light on "a command was sent" would
 * lie. */
HC_API int32_t hc_session_sink_available(hc_session* session);

/* --- bindings ----------------------------------------------------------- */

/* The current bindings, serialized in the file format. Returns the length
 * needed including the NUL; writes nothing if `capacity` is too small.
 *
 * Round-tripping through here is lossless and tested, which is why a UI must
 * not write the format itself -- two definitions drift on the first edit to
 * either. */
HC_API int32_t hc_session_bindings_text(hc_session* session, char* out,
                                 int32_t capacity);

/* Parses `text` and applies it, dropping smoothing state with the old set.
 * Returns 1 on success; on failure returns 0, writes the parser's complaint
 * (which names the line) to `error`, and LEAVES THE PREVIOUS BINDINGS IN
 * PLACE -- a rejected edit must not disarm a working configuration. */
HC_API int32_t hc_session_set_bindings_text(hc_session* session, const char* text,
                                     char* error, int32_t error_capacity);

/* The configuration compiled into the library. Static storage, never NULL,
 * and not to be freed. */
HC_API const char* hc_default_bindings_text(void);

/* --- discovery and errors ----------------------------------------------- */

typedef struct {
  int32_t index;    /* pass as hc_config::camera_index */
  char path[64];    /* e.g. "/dev/video0" */
  char name[128];   /* as the driver reports it */
} hc_camera;

/* Fills up to `capacity` entries and returns how many exist -- which may
 * exceed `capacity`, so a caller can size and retry. Linux only; returns 0
 * elsewhere rather than pretending. */
HC_API int32_t hc_enumerate_cameras(hc_camera* out, int32_t capacity);

/* The last failure on this session, NUL-terminated. Returns the length
 * written. Cleared by the next successful call that sets it. */
HC_API int32_t hc_session_last_error(hc_session* session, char* out,
                              int32_t capacity);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* HC_HAND_CONTROLLER_H */
