// The C ABI over hc::Controller. See include/hc/hand_controller.h for the
// contract; this file is the plumbing behind it.
//
// Three jobs, and nothing else belongs here:
//
//   1. Translate plain C structs to and from the C++ types. No decisions --
//      what a gesture means lives in hc_control, and a translation layer that
//      acquired an `if` about a gesture would be the second copy of that
//      decision.
//
//   2. Own the frame loop on a WORKER THREAD, because Step() costs ~11 ms and
//      no UI thread can pay that ninety times a second. The caller polls the
//      latest published snapshot and never blocks on a frame.
//
//   3. Let nothing escape: no exception, no C++ type, no pointer the caller
//      would have to free.

#include "hc/hand_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <dirent.h>
#include <fstream>

#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/controller.hpp"
#include "hc/frame_source.hpp"
#include "hc/gesture_event.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/hand_tracker_onnx.hpp"
#include "hc/latest_frame_source.hpp"
#include "hc/logging_sink.hpp"
#include "hc/metrics.hpp"
#include "hc/queued_sink.hpp"
#include "hc/synthetic_frame_source.hpp"

#include "opencv_frame_source.hpp"
#ifdef __linux__
#include "uinput_sink.hpp"
#endif

namespace {

// THE ENUM VALUES ARE PART OF THE ABI. A reordered C++ enum would silently
// remap every gesture a UI displays, so each one is pinned here rather than
// trusted to a comment.
static_assert(static_cast<int>(hc::Handedness::Unknown) ==
                  HC_HANDEDNESS_UNKNOWN, "ABI: Handedness::Unknown");
static_assert(static_cast<int>(hc::Handedness::Left) == HC_HANDEDNESS_LEFT,
              "ABI: Handedness::Left");
static_assert(static_cast<int>(hc::Handedness::Right) == HC_HANDEDNESS_RIGHT,
              "ABI: Handedness::Right");

static_assert(static_cast<int>(hc::GestureKind::None) == HC_GESTURE_NONE, "");
static_assert(static_cast<int>(hc::GestureKind::Pinch) == HC_GESTURE_PINCH, "");
static_assert(static_cast<int>(hc::GestureKind::Fist) == HC_GESTURE_FIST, "");
static_assert(static_cast<int>(hc::GestureKind::OpenPalm) ==
                  HC_GESTURE_OPEN_PALM, "");
static_assert(static_cast<int>(hc::GestureKind::PointIndex) ==
                  HC_GESTURE_POINT_INDEX, "");
static_assert(static_cast<int>(hc::GestureKind::TwoFingers) ==
                  HC_GESTURE_TWO_FINGERS, "");
static_assert(static_cast<int>(hc::GestureKind::ThumbUp) ==
                  HC_GESTURE_THUMB_UP, "");
static_assert(static_cast<int>(hc::GestureKind::PinkyOnly) ==
                  HC_GESTURE_PINKY_ONLY, "");
static_assert(static_cast<int>(hc::GestureKind::FourFingers) ==
                  HC_GESTURE_FOUR_FINGERS, "");
static_assert(hc::kGestureKindCount == 9,
              "a gesture was added: extend hc_gesture_kind and this check");

static_assert(static_cast<int>(hc::GestureEvent::Phase::Began) ==
                  HC_PHASE_BEGAN, "");
static_assert(static_cast<int>(hc::GestureEvent::Phase::Held) == HC_PHASE_HELD,
              "");
static_assert(static_cast<int>(hc::GestureEvent::Phase::Ended) ==
                  HC_PHASE_ENDED, "");

static_assert(static_cast<int>(hc::Command::Kind::None) == HC_COMMAND_NONE, "");
static_assert(static_cast<int>(hc::Command::Kind::MoveCursorRelative) ==
                  HC_COMMAND_MOVE_CURSOR, "");
static_assert(static_cast<int>(hc::Command::Kind::Click) == HC_COMMAND_CLICK,
              "");
static_assert(static_cast<int>(hc::Command::Kind::DoubleClick) ==
                  HC_COMMAND_DOUBLE_CLICK, "");
static_assert(static_cast<int>(hc::Command::Kind::Scroll) == HC_COMMAND_SCROLL,
              "");
static_assert(static_cast<int>(hc::Command::Kind::KeyPress) ==
                  HC_COMMAND_KEY_PRESS, "");
static_assert(static_cast<int>(hc::Command::Kind::KeyRelease) ==
                  HC_COMMAND_KEY_RELEASE, "");

static_assert(static_cast<int>(hc::MouseButton::Left) == HC_BUTTON_LEFT, "");
static_assert(static_cast<int>(hc::MouseButton::Right) == HC_BUTTON_RIGHT, "");
static_assert(static_cast<int>(hc::MouseButton::Middle) == HC_BUTTON_MIDDLE, "");

static_assert(static_cast<int>(hc::Key::ArrowUp) == HC_KEY_ARROW_UP, "");
static_assert(static_cast<int>(hc::Key::ArrowDown) == HC_KEY_ARROW_DOWN, "");
static_assert(static_cast<int>(hc::Key::ArrowLeft) == HC_KEY_ARROW_LEFT, "");
static_assert(static_cast<int>(hc::Key::ArrowRight) == HC_KEY_ARROW_RIGHT, "");
static_assert(static_cast<int>(hc::Key::PrintScreen) ==
                  HC_KEY_PRINT_SCREEN, "");
static_assert(static_cast<int>(hc::Key::Escape) == HC_KEY_ESCAPE, "");
static_assert(static_cast<int>(hc::Key::Enter) == HC_KEY_ENTER, "");
static_assert(static_cast<int>(hc::Key::Space) == HC_KEY_SPACE, "");
static_assert(static_cast<int>(hc::Key::Tab) == HC_KEY_TAB, "");

static_assert(std::tuple_size<decltype(hc::HandObservation::landmarks)>::value
                  == HC_LANDMARK_COUNT,
              "ABI: the model's landmark count changed");

// Writes `text` into a caller buffer, NUL-terminated and truncated to fit.
// Returns the bytes written including the NUL, or 0 for no buffer.
int32_t WriteString(const std::string& text, char* out, int32_t capacity) {
  if (out == nullptr || capacity <= 0) return 0;

  const std::size_t room = static_cast<std::size_t>(capacity) - 1;
  const std::size_t n = std::min(room, text.size());
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
  return static_cast<int32_t>(n + 1);
}

// Records how long the inner source's Next() took, so the FFI can report a
// latency that EXCLUDES time spent waiting on the camera.
//
// Without this, a 30 fps camera makes every frame look like it cost 33 ms of
// work when most of that was the pipeline sitting idle -- which would read as
// a performance problem that does not exist. Metrics aggregates CaptureWait
// across the run and cannot answer "for this frame".
class TimedSource : public hc::IFrameSource {
 public:
  explicit TimedSource(std::unique_ptr<hc::IFrameSource> inner)
      : inner_(std::move(inner)) {}

  bool Next(hc::Frame& out) override {
    const auto start = std::chrono::steady_clock::now();
    const bool ok = inner_->Next(out);
    const auto end = std::chrono::steady_clock::now();
    last_wait_ms_ =
        std::chrono::duration<double, std::milli>(end - start).count();
    return ok;
  }

  void Close() override { inner_->Close(); }

  double last_wait_ms() const { return last_wait_ms_; }

 private:
  std::unique_ptr<hc::IFrameSource> inner_;
  double last_wait_ms_ = 0.0;
};

// One published frame, in C-ABI-shaped vectors so poll is a memcpy per array
// rather than a conversion under the lock.
struct Snapshot {
  std::vector<hc_hand> hands;
  std::vector<hc_event> events;
  std::vector<hc_command> commands;

  // RGBA, tightly packed. Converted on the worker thread, because a caller
  // polling at 60 Hz should not be paying for a colour conversion.
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;

  std::int64_t timestamp_us = 0;
  std::uint64_t frame_index = 0;
  std::uint64_t frames_dropped = 0;
  double fps = 0.0;
  double latency_ms = 0.0;
};

}  // namespace

// Deliberately outside the anonymous namespace: it is the type the opaque
// handle in the header points at.
struct hc_session {
  std::unique_ptr<hc::Controller> controller;
  hc::LatestFrameSource* capture = nullptr;  // Borrowed; owned by controller.
  TimedSource* timed = nullptr;              // Borrowed, likewise.
  hc::Metrics metrics;

  // Guards the controller. The worker holds it while stepping, so a control
  // call from the caller's thread can wait up to one frame (~11 ms) -- which
  // is the cost of not having two threads touch a class documented as
  // single-threaded.
  std::mutex controller_mutex;

  std::mutex snapshot_mutex;
  Snapshot snapshot;
  bool has_snapshot = false;

  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};

  // Reusable scratch, so the steady state allocates nothing.
  std::vector<hc::HandObservation> observations;
  std::vector<hc::GestureEvent> events;
  std::vector<hc::Command> commands;
  std::vector<std::uint32_t> origins;

  std::mutex error_mutex;
  std::string last_error;

  int sink_kind = HC_SINK_NONE;
  int screen_width = 0;
  int screen_height = 0;

  void SetError(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex);
    last_error = message;
  }
};

namespace {

// Builds the sink a kind asks for. Returns false with a reason rather than
// substituting a working sink for a failed one: a UI that asked to inject and
// silently got preview mode would show a green light over a dead cursor.
bool MakeSink(hc_session* session, int kind,
              std::unique_ptr<hc::IOutputSink>& out, std::string& error) {
  constexpr std::size_t kQueueCapacity = 256;

  switch (kind) {
    case HC_SINK_NONE:
      out.reset();
      return true;

    case HC_SINK_LOG:
      // Queued: it writes to a stream, and a stream can block.
      out = std::make_unique<hc::QueuedSink>(
          std::make_unique<hc::LoggingSink>(), kQueueCapacity);
      return true;

    case HC_SINK_UINPUT: {
#ifdef __linux__
      int width = session->screen_width;
      int height = session->screen_height;
      if (width <= 0 || height <= 0) {
        if (!hc::DetectScreenSize(width, height)) {
          width = 1920;
          height = 1080;
        }
      }
      std::string why;
      auto device = hc::UinputSink::Create(width, height, why);
      if (!device) {
        error = why;
        return false;
      }
      out = std::make_unique<hc::QueuedSink>(std::move(device),
                                             kQueueCapacity);
      return true;
#else
      error = "the uinput sink is Linux only";
      return false;
#endif
    }

    default:
      error = "unknown sink kind " + std::to_string(kind);
      return false;
  }
}

std::unique_ptr<hc::IFrameSource> MakeSource(const hc_config& config,
                                             std::string& error) {
  switch (config.source) {
    case HC_SOURCE_SYNTHETIC: {
      const int w = config.synthetic_width > 0 ? config.synthetic_width : 640;
      const int h = config.synthetic_height > 0 ? config.synthetic_height : 480;
      return std::make_unique<hc::SyntheticFrameSource>(w, h,
                                                        /*frame_count=*/-1);
    }

    case HC_SOURCE_VIDEO: {
      if (config.video_path == nullptr || config.video_path[0] == '\0') {
        error = "video source selected but no path was given";
        return nullptr;
      }
      auto source = hc::OpenCvFrameSource::OpenFile(config.video_path);
      if (!source) {
        error = std::string("could not open video file ") + config.video_path;
      }
      return source;
    }

    case HC_SOURCE_CAMERA: {
      hc::CameraOptions camera;
      camera.device_index = config.camera_index;
      auto source = hc::OpenCvFrameSource::OpenCamera(camera);
      if (!source) {
        // The message a user most often needs, and the diagnosis that cost
        // this project an hour: OpenCV reports only EBUSY when another
        // process holds the device.
        error = "could not open camera " + std::to_string(config.camera_index) +
                " (it may be in use by another process; check /proc/*/fd for "
                "/dev/video*)";
      }
      return source;
    }

    default:
      error = "unknown source kind " + std::to_string(config.source);
      return nullptr;
  }
}

// Converts one frame to tightly packed RGBA.
//
// Tight packing matters: `stride` may exceed width * 3 (V4L2 and MJPEG decode
// both pad rows), and a UI uploading the padding gets a sheared image.
void ToRgba(const hc::ImageView& view, std::vector<std::uint8_t>& out) {
  if (view.data == nullptr || view.width <= 0 || view.height <= 0) {
    out.clear();
    return;
  }

  const bool bgr = view.format == hc::PixelFormat::Bgr8;
  out.resize(static_cast<std::size_t>(view.width) * view.height * 4);

  for (int y = 0; y < view.height; ++y) {
    const std::uint8_t* src =
        view.data + static_cast<std::size_t>(y) * view.stride;
    std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * view.width * 4;
    for (int x = 0; x < view.width; ++x) {
      const std::uint8_t a = src[x * 3 + 0];
      const std::uint8_t b = src[x * 3 + 1];
      const std::uint8_t c = src[x * 3 + 2];
      dst[x * 4 + 0] = bgr ? c : a;  // R
      dst[x * 4 + 1] = b;            // G
      dst[x * 4 + 2] = bgr ? a : c;  // B
      dst[x * 4 + 3] = 255;
    }
  }
}

void Publish(hc_session* session, double step_ms, double wait_ms) {
  Snapshot next;
  next.hands.reserve(session->observations.size());
  for (const hc::HandObservation& o : session->observations) {
    hc_hand hand{};
    hand.track_id = o.track_id;
    hand.handedness = static_cast<int32_t>(o.handedness);
    hand.handedness_score = o.handedness_score;
    hand.presence = o.presence;
    hand.timestamp_us = o.timestamp_us;
    for (std::size_t i = 0; i < o.landmarks.size(); ++i) {
      hand.landmarks[i] = hc_point{o.landmarks[i].x, o.landmarks[i].y};
    }
    next.hands.push_back(hand);
  }

  next.events.reserve(session->events.size());
  for (const hc::GestureEvent& e : session->events) {
    hc_event ev{};
    ev.track_id = e.track_id;
    ev.handedness = static_cast<int32_t>(e.handedness);
    ev.gesture = static_cast<int32_t>(e.kind);
    ev.phase = static_cast<int32_t>(e.phase);
    ev.position = hc_point{e.position.x, e.position.y};
    ev.delta = hc_point{e.delta.x, e.delta.y};
    ev.timestamp_us = e.timestamp_us;
    next.events.push_back(ev);
  }

  const bool delivered =
      session->controller->SinkAvailable() && !session->controller->Paused();

  next.commands.reserve(session->commands.size());
  for (std::size_t i = 0; i < session->commands.size(); ++i) {
    const hc::Command& c = session->commands[i];
    hc_command cmd{};
    cmd.kind = static_cast<int32_t>(c.kind);
    cmd.delta = hc_point{c.delta.x, c.delta.y};
    cmd.button = static_cast<int32_t>(c.button);
    cmd.key = static_cast<int32_t>(c.key);
    cmd.origin_event = i < session->origins.size()
                           ? static_cast<int32_t>(session->origins[i])
                           : -1;
    cmd.delivered = delivered ? 1 : 0;
    next.commands.push_back(cmd);
  }

  const hc::Frame& frame = session->controller->LastFrame();
  ToRgba(frame.view, next.rgba);
  next.width = frame.view.width;
  next.height = frame.view.height;
  next.timestamp_us = frame.timestamp_us;
  next.frame_index = session->controller->ProcessedFrames();
  next.frames_dropped =
      session->capture != nullptr ? session->capture->dropped() : 0;

  // Work, not wall time: the camera wait is excluded, so this answers "how
  // long did the pipeline take" rather than "how fast is the camera".
  next.latency_ms = std::max(0.0, step_ms - wait_ms);

  {
    std::lock_guard<std::mutex> lock(session->snapshot_mutex);
    // Smoothed, because a single frame's wall time swings wildly and a
    // readout that flickers between 60 and 140 tells a user nothing. One
    // pole, weighted towards history.
    const double instant = step_ms > 0.0 ? 1000.0 / step_ms : 0.0;
    next.fps = session->has_snapshot
                   ? 0.9 * session->snapshot.fps + 0.1 * instant
                   : instant;
    session->snapshot = std::move(next);
    session->has_snapshot = true;
  }
}

void WorkerLoop(hc_session* session) {
  while (!session->stop_requested.load()) {
    const auto start = std::chrono::steady_clock::now();

    bool stepped = false;
    {
      std::lock_guard<std::mutex> lock(session->controller_mutex);
      stepped = session->controller->Step();
      if (stepped) {
        session->controller->PollObservations(session->observations);
        session->controller->PollEvents(session->events);
        session->controller->PollCommands(session->commands);
        session->controller->PollCommandOrigins(session->origins);
      }
    }

    if (!stepped) {
      // The source ended. The session is over but NOT stopped -- the caller
      // still owns the shutdown, exactly as Controller documents.
      session->running.store(false);
      return;
    }

    const auto end = std::chrono::steady_clock::now();
    const double step_ms =
        std::chrono::duration<double, std::milli>(end - start).count();

    Publish(session, step_ms,
            session->timed != nullptr ? session->timed->last_wait_ms() : 0.0);
  }
  session->running.store(false);
}

}  // namespace

extern "C" {

int32_t hc_abi_version(void) { return HC_ABI_VERSION; }

const char* hc_handedness_name(int32_t handedness) {
  switch (handedness) {
    case HC_HANDEDNESS_LEFT: return "left";
    case HC_HANDEDNESS_RIGHT: return "right";
    default: return "unknown";
  }
}

const char* hc_gesture_name(int32_t gesture) {
  if (gesture < 0 || gesture >= static_cast<int32_t>(hc::kGestureKindCount)) {
    return "none";
  }
  return hc::GestureKindName(static_cast<hc::GestureKind>(gesture));
}

const char* hc_phase_name(int32_t phase) {
  if (phase < HC_PHASE_BEGAN || phase > HC_PHASE_ENDED) return "began";
  return hc::GesturePhaseName(static_cast<hc::GestureEvent::Phase>(phase));
}

const char* hc_command_name(int32_t kind) {
  if (kind < HC_COMMAND_NONE || kind > HC_COMMAND_KEY_RELEASE) return "none";
  return hc::CommandKindName(static_cast<hc::Command::Kind>(kind));
}

const char* hc_button_name(int32_t button) {
  if (button < HC_BUTTON_LEFT || button > HC_BUTTON_MIDDLE) return "left";
  return hc::MouseButtonName(static_cast<hc::MouseButton>(button));
}

const char* hc_key_name(int32_t key) {
  if (key < HC_KEY_ARROW_UP || key > HC_KEY_TAB) return "escape";
  return hc::KeyName(static_cast<hc::Key>(key));
}

const char* hc_sink_name(int32_t sink) {
  switch (sink) {
    case HC_SINK_LOG: return "log";
    case HC_SINK_UINPUT: return "uinput";
    default: return "none";
  }
}

const char* hc_default_bindings_text(void) { return hc::DefaultBindingsText(); }

void hc_config_defaults(hc_config* out) {
  if (out == nullptr) return;

  hc_config defaults{};
  defaults.source = HC_SOURCE_CAMERA;
  defaults.camera_index = 0;
  defaults.video_path = nullptr;
  defaults.synthetic_width = 640;
  defaults.synthetic_height = 480;
  defaults.max_hands = 2;
  defaults.threads = 1;
  defaults.bindings_text = nullptr;
  defaults.sensitivity = 0.0f;  // 0 means "leave the file's own value".
  defaults.smoothing = -1.0f;   // Negative, because 0 is a legal smoothing.
  // No sink by default. Development happens on the same desktop this would
  // seize, and a library that grabs the cursor on creation is hostile.
  defaults.sink = HC_SINK_NONE;
  defaults.screen_width = 0;
  defaults.screen_height = 0;
  *out = defaults;
}

hc_session* hc_session_create(const hc_config* config, char* error,
                              int32_t error_capacity) {
  if (config == nullptr) {
    WriteString("config must not be null", error, error_capacity);
    return nullptr;
  }

  try {
    auto session = std::make_unique<hc_session>();
    session->screen_width = config->screen_width;
    session->screen_height = config->screen_height;

    // Bindings first: a configuration error should be reported before a
    // camera is touched or a model is loaded.
    hc::BindingSet bindings;
    const char* text = (config->bindings_text != nullptr &&
                        config->bindings_text[0] != '\0')
                           ? config->bindings_text
                           : hc::DefaultBindingsText();
    std::string parse_error;
    if (!hc::ParseBindings(text, bindings, parse_error)) {
      WriteString(parse_error, error, error_capacity);
      return nullptr;
    }
    if (config->sensitivity > 0.0f) bindings.sensitivity = config->sensitivity;
    if (config->smoothing >= 0.0f) bindings.smoothing = config->smoothing;

    std::string source_error;
    auto raw_source = MakeSource(*config, source_error);
    if (!raw_source) {
      WriteString(source_error, error, error_capacity);
      return nullptr;
    }

    // Timed, then latest-wins. Order matters: TimedSource must measure the
    // wait the CONSUMER experiences, which is the wait on the latest-frame
    // wrapper, not on the device behind it.
    auto latest =
        std::make_unique<hc::LatestFrameSource>(std::move(raw_source));
    session->capture = latest.get();
    auto timed = std::make_unique<TimedSource>(std::move(latest));
    session->timed = timed.get();

    hc::HandTrackerConfig tracker_config;
    tracker_config.palm_model_path = HC_PALM_MODEL_PATH;
    tracker_config.landmark_model_path = HC_LANDMARK_MODEL_PATH;
    tracker_config.max_hands = config->max_hands > 0 ? config->max_hands : 2;
    tracker_config.intra_op_threads = config->threads > 0 ? config->threads : 1;

    // Throws if either model is missing or has an unexpected layout: a
    // startup failure with a useful message, not a crash.
    auto tracker =
        std::make_unique<hc::HandTrackerOnnx>(tracker_config, session->metrics);

    std::unique_ptr<hc::IOutputSink> sink;
    std::string sink_error;
    if (!MakeSink(session.get(), config->sink, sink, sink_error)) {
      WriteString(sink_error, error, error_capacity);
      return nullptr;
    }
    session->sink_kind = config->sink;

    hc::ControllerConfig controller_config;
    controller_config.bindings = std::move(bindings);

    session->controller = std::make_unique<hc::Controller>(
        std::move(timed), std::move(tracker), std::move(sink),
        std::move(controller_config), &session->metrics);

    WriteString("", error, error_capacity);
    return session.release();
  } catch (const std::exception& e) {
    WriteString(e.what(), error, error_capacity);
    return nullptr;
  } catch (...) {
    WriteString("unknown failure creating the session", error, error_capacity);
    return nullptr;
  }
}

void hc_session_destroy(hc_session* session) {
  if (session == nullptr) return;
  hc_session_stop(session);
  delete session;
}

int32_t hc_session_start(hc_session* session) {
  if (session == nullptr) return 0;

  try {
    if (session->running.load()) {
      session->SetError("the session is already running");
      return 0;
    }
    {
      std::lock_guard<std::mutex> lock(session->controller_mutex);
      if (!session->controller->Start()) {
        session->SetError(
            "the session could not start; a stopped session does not restart");
        return 0;
      }
    }

    session->stop_requested.store(false);
    session->running.store(true);
    session->worker = std::thread(WorkerLoop, session);
    return 1;
  } catch (const std::exception& e) {
    session->SetError(e.what());
    session->running.store(false);
    return 0;
  } catch (...) {
    session->SetError("unknown failure starting the session");
    session->running.store(false);
    return 0;
  }
}

void hc_session_stop(hc_session* session) {
  if (session == nullptr) return;

  try {
    session->stop_requested.store(true);
    if (session->worker.joinable()) session->worker.join();
    session->running.store(false);

    std::lock_guard<std::mutex> lock(session->controller_mutex);
    if (session->controller) session->controller->Stop();
  } catch (...) {
    // Nothing useful to report from a shutdown path, and throwing out of
    // here would cross the boundary.
  }
}

int32_t hc_session_running(hc_session* session) {
  if (session == nullptr) return 0;
  return session->running.load() ? 1 : 0;
}

int32_t hc_session_poll(hc_session* session, hc_snapshot* snapshot) {
  if (session == nullptr || snapshot == nullptr) return 0;

  std::lock_guard<std::mutex> lock(session->snapshot_mutex);
  if (!session->has_snapshot) {
    snapshot->hand_count = 0;
    snapshot->event_count = 0;
    snapshot->command_count = 0;
    return 0;
  }

  const Snapshot& s = session->snapshot;

  const auto copy = [](auto* dst, int32_t capacity, const auto& src) -> int32_t {
    if (dst == nullptr || capacity <= 0) return 0;
    const std::size_t n =
        std::min(static_cast<std::size_t>(capacity), src.size());
    if (n > 0) std::memcpy(dst, src.data(), n * sizeof(src[0]));
    return static_cast<int32_t>(n);
  };

  snapshot->hand_count = copy(snapshot->hands, snapshot->hand_capacity, s.hands);
  snapshot->event_count =
      copy(snapshot->events, snapshot->event_capacity, s.events);
  snapshot->command_count =
      copy(snapshot->commands, snapshot->command_capacity, s.commands);

  snapshot->timestamp_us = s.timestamp_us;
  snapshot->frame_index = s.frame_index;
  snapshot->frames_dropped = s.frames_dropped;
  snapshot->fps = s.fps;
  snapshot->latency_ms = s.latency_ms;
  snapshot->frame_width = s.width;
  snapshot->frame_height = s.height;
  return 1;
}

int32_t hc_session_copy_frame(hc_session* session, uint8_t* dst,
                              int32_t capacity, int32_t* width,
                              int32_t* height) {
  if (session == nullptr) return 0;

  std::lock_guard<std::mutex> lock(session->snapshot_mutex);
  const Snapshot& s = session->snapshot;
  if (!session->has_snapshot || s.rgba.empty()) return 0;

  if (width != nullptr) *width = s.width;
  if (height != nullptr) *height = s.height;

  const int32_t needed = static_cast<int32_t>(s.rgba.size());
  if (dst == nullptr || capacity < needed) return needed;

  std::memcpy(dst, s.rgba.data(), s.rgba.size());
  return needed;
}

void hc_session_set_paused(hc_session* session, int32_t paused) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->controller_mutex);
  if (session->controller) session->controller->SetPaused(paused != 0);
}

int32_t hc_session_paused(hc_session* session) {
  if (session == nullptr) return 0;
  std::lock_guard<std::mutex> lock(session->controller_mutex);
  return (session->controller && session->controller->Paused()) ? 1 : 0;
}

int32_t hc_session_set_sink(hc_session* session, int32_t sink, char* error,
                            int32_t error_capacity) {
  if (session == nullptr) {
    WriteString("session must not be null", error, error_capacity);
    return 0;
  }

  try {
    std::unique_ptr<hc::IOutputSink> replacement;
    std::string why;

    // Built BEFORE the swap, so a sink that cannot be created leaves the
    // previous one in place. Otherwise a refused uinput would silently
    // downgrade a running session to preview mode.
    if (!MakeSink(session, sink, replacement, why)) {
      session->SetError(why);
      WriteString(why, error, error_capacity);
      return 0;
    }

    {
      std::lock_guard<std::mutex> lock(session->controller_mutex);
      session->controller->SetSink(std::move(replacement));
    }
    session->sink_kind = sink;
    WriteString("", error, error_capacity);
    return 1;
  } catch (const std::exception& e) {
    session->SetError(e.what());
    WriteString(e.what(), error, error_capacity);
    return 0;
  } catch (...) {
    WriteString("unknown failure swapping the sink", error, error_capacity);
    return 0;
  }
}

int32_t hc_session_sink_available(hc_session* session) {
  if (session == nullptr) return 0;
  std::lock_guard<std::mutex> lock(session->controller_mutex);
  return (session->controller && session->controller->SinkAvailable()) ? 1 : 0;
}

int32_t hc_session_bindings_text(hc_session* session, char* out,
                                 int32_t capacity) {
  if (session == nullptr) return 0;

  try {
    std::string text;
    {
      std::lock_guard<std::mutex> lock(session->controller_mutex);
      text = hc::SerializeBindings(session->controller->Bindings());
    }
    const int32_t needed = static_cast<int32_t>(text.size()) + 1;
    if (out == nullptr || capacity < needed) return needed;
    return WriteString(text, out, capacity);
  } catch (...) {
    return 0;
  }
}

int32_t hc_session_set_bindings_text(hc_session* session, const char* text,
                                     char* error, int32_t error_capacity) {
  if (session == nullptr || text == nullptr) {
    WriteString("session and text must not be null", error, error_capacity);
    return 0;
  }

  try {
    hc::BindingSet parsed;
    std::string parse_error;
    // Parsed BEFORE anything is replaced: a rejected edit must not disarm a
    // working configuration.
    if (!hc::ParseBindings(text, parsed, parse_error)) {
      session->SetError(parse_error);
      WriteString(parse_error, error, error_capacity);
      return 0;
    }

    {
      std::lock_guard<std::mutex> lock(session->controller_mutex);
      session->controller->SetBindings(std::move(parsed));
    }
    WriteString("", error, error_capacity);
    return 1;
  } catch (const std::exception& e) {
    WriteString(e.what(), error, error_capacity);
    return 0;
  } catch (...) {
    WriteString("unknown failure applying the bindings", error,
                error_capacity);
    return 0;
  }
}

int32_t hc_enumerate_cameras(hc_camera* out, int32_t capacity) {
#ifdef __linux__
  DIR* dir = opendir("/dev");
  if (dir == nullptr) return 0;

  // Collected then sorted, because readdir order is arbitrary and a camera
  // list that reshuffles between calls would move the user's selection.
  std::vector<int> indices;
  while (const struct dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.rfind("video", 0) != 0) continue;
    const std::string digits = name.substr(5);
    if (digits.empty() ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    indices.push_back(std::atoi(digits.c_str()));
  }
  closedir(dir);
  std::sort(indices.begin(), indices.end());

  int32_t written = 0;
  for (const int index : indices) {
    if (out != nullptr && written < capacity) {
      hc_camera camera{};
      camera.index = index;
      const std::string path = "/dev/video" + std::to_string(index);
      std::snprintf(camera.path, sizeof(camera.path), "%s", path.c_str());

      // The driver's own name, which is how a user tells two cameras apart.
      std::string label;
      std::ifstream file("/sys/class/video4linux/video" +
                         std::to_string(index) + "/name");
      if (file) std::getline(file, label);
      if (label.empty()) label = path;
      std::snprintf(camera.name, sizeof(camera.name), "%s", label.c_str());

      out[written] = camera;
    }
    ++written;
  }
  return written;
#else
  (void)out;
  (void)capacity;
  return 0;
#endif
}

int32_t hc_session_last_error(hc_session* session, char* out,
                              int32_t capacity) {
  if (session == nullptr) return 0;
  std::lock_guard<std::mutex> lock(session->error_mutex);
  return WriteString(session->last_error, out, capacity);
}

}  // extern "C"
