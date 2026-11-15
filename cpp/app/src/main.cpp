// The application shell: build the pieces the command line asks for, hand
// them to a Controller, and drive it one frame at a time.
//
// Deliberately thin, and thinner than it was in phase 1: the loop itself now
// lives in hc::Controller so that a GUI, a .NET host or a server can run the
// same session without reimplementing it. What is left here is composition,
// the preview window, and the report -- the three things that are genuinely
// specific to being a command-line program.

#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cli.hpp"
#include "opencv_frame_source.hpp"
#include "overlay.hpp"
#include "sendinput_sink.hpp"
#include "uinput_sink.hpp"

#include "hc/bindings.hpp"
#include "hc/controller.hpp"
#include "hc/frame_source.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/hand_tracker_onnx.hpp"
#include "hc/latest_frame_source.hpp"
#include "hc/logging_sink.hpp"
#include "hc/metrics.hpp"
#include "hc/null_sink.hpp"
#include "hc/queued_sink.hpp"
#include "hc/synthetic_frame_source.hpp"

namespace {

// Commands held before the dispatch queue starts evicting. Generous: at ~90
// frames per second a held gesture produces one command per frame, so this is
// seconds of backlog, and a queue that is seconds behind is a sink that has
// failed rather than one that is busy. QueuedSink::DroppedCount is what would
// show that.
constexpr std::size_t kSinkQueueCapacity = 256;

// Builds the frame source the options ask for, or nullptr with a message on
// stderr. The failure cases differ per source, so each one explains itself
// rather than sharing a vague "could not open source".
std::unique_ptr<hc::IFrameSource> MakeSource(const hc::CliOptions& opts) {
  switch (opts.source) {
    case hc::SourceKind::Synthetic:
      return std::make_unique<hc::SyntheticFrameSource>(
          opts.synthetic_width, opts.synthetic_height, /*frame_count=*/-1);

    case hc::SourceKind::Video: {
      auto source = hc::OpenCvFrameSource::OpenFile(opts.video_path);
      if (!source) {
        std::cerr << "error: could not open video file " << opts.video_path
                  << "\n";
      }
      return source;
    }

    case hc::SourceKind::Camera: {
      hc::CameraOptions camera;
      camera.device_index = opts.camera_index;
      auto source = hc::OpenCvFrameSource::OpenCamera(camera);
      if (!source) {
        std::cerr << "error: could not open camera " << opts.camera_index
                  << "\n";
      }
      return source;
    }
  }
  return nullptr;
}

// The bindings the run will use: the file if one was named, otherwise the
// configuration compiled into hc_control.
//
// A missing or invalid file is FATAL rather than a fallback to the default. A
// user who passes --bindings has a configuration in mind, and silently running
// someone else's bindings because theirs had a typo is worse than refusing to
// start -- the gestures would work, just not the ones they wrote.
bool LoadBindings(const hc::CliOptions& opts, hc::BindingSet& out) {
  std::string text = hc::DefaultBindingsText();
  if (!opts.bindings_path.empty()) {
    std::string read_error;
    if (!hc::ReadTextFile(opts.bindings_path, text, read_error)) {
      std::cerr << "error: " << read_error << "\n";
      return false;
    }
  }

  std::string error;
  if (!hc::ParseBindings(text, out, error)) {
    const std::string where =
        opts.bindings_path.empty() ? "the compiled-in default bindings"
                                   : opts.bindings_path;
    std::cerr << "error: " << where << ": " << error << "\n";
    return false;
  }

  // Applied AFTER parsing so a flag beats the file, and only when given, so
  // absence does not overwrite the file's own `set` lines.
  if (opts.has_sensitivity) out.sensitivity = opts.sensitivity;
  if (opts.has_smoothing) out.smoothing = opts.smoothing;
  return true;
}

// Builds the sink, or nullptr with a message. A null return is fatal; a
// Controller with no sink is a legitimate configuration but it is not what
// `--sink log` asked for, and quietly substituting one would hide the failure.
std::unique_ptr<hc::IOutputSink> MakeSink(const hc::CliOptions& opts) {
  switch (hc::ResolveSink(opts)) {
    case hc::SinkKind::Null:
      // Not wrapped in a queue: discarding cannot block, and a thread whose
      // only job is to discard is a thread to go wrong for nothing.
      return std::make_unique<hc::NullSink>();

    case hc::SinkKind::Log:
      // Queued because it writes to a terminal, and a terminal can block --
      // a piped stdout with nobody reading is enough to stall the frame loop.
      return std::make_unique<hc::QueuedSink>(
          std::make_unique<hc::LoggingSink>(&std::cout), kSinkQueueCapacity);

    case hc::SinkKind::Uinput: {
#ifdef __linux__
      // The one backend verified to inject on this machine. Queued, because a
      // write to a device node can block and the frame loop must not.
      //
      // The screen size is read from /sys/class/drm rather than from a display
      // server: this sink deliberately talks to none, and asking Wayland for a
      // resolution would reintroduce the protocol dependency it exists to
      // avoid. A guess is used if that fails, and SAID OUT LOUD, because a
      // wrong one makes the cursor the wrong speed and nothing else.
      int width = 1920;
      int height = 1080;
      if (!hc::DetectScreenSize(width, height)) {
        std::cerr << "warning: could not read a screen size from "
                     "/sys/class/drm; assuming "
                  << width << "x" << height
                  << ". Cursor speed will be off; use --sensitivity.\n";
      }

      std::string sink_error;
      auto device = hc::UinputSink::Create(width, height, sink_error);
      if (!device) {
        std::cerr << "error: " << sink_error << "\n";
        return nullptr;
      }
      std::cout << "uinput device: " << hc::UinputSink::DeviceName();
      if (!device->DeviceNode().empty()) {
        std::cout << " at " << device->DeviceNode();
      }
      std::cout << "\n";

      return std::make_unique<hc::QueuedSink>(std::move(device),
                                              kSinkQueueCapacity);
#else
      std::cerr << "error: --sink uinput is Linux only.\n";
      return nullptr;
#endif
    }

    case hc::SinkKind::SendInput:
#ifdef _WIN32
      // Written blind and never run by anyone -- see sendinput_sink.hpp. It is
      // reachable rather than dead code, which is the most that can honestly
      // be claimed for it from here.
      if (auto sink = hc::SendInputSink::Create()) return sink;
      std::cerr << "error: SendInput reports no desktop to inject into.\n";
      return nullptr;
#else
      std::cerr << "error: --sink sendinput is Windows only.\n";
      return nullptr;
#endif

    case hc::SinkKind::XTest:
      // Task 9 is not written: the probe (cpp/tools/xtest_probe) shows XTest
      // on this machine returning success and moving nothing, because the
      // session is Wayland. Refusing here is the honest answer -- a sink that
      // reports success and does nothing is the exact failure the project is
      // trying not to ship.
      std::cerr << "error: the xtest sink is not built yet.\n"
                   "  Run cpp/build/tools/xtest_probe: on this machine XTest "
                   "reports success and\n"
                   "  moves nothing, because the session is Wayland. Use "
                   "--sink log to see what\n"
                   "  the gestures would do.\n";
      return nullptr;
  }
  return nullptr;
}

// What the sink is doing, in the user's terms rather than the Controller's.
//
// The Controller knows only whether the sink it was handed can DELIVER; which
// sinks inject input is this file's knowledge, because this file is what chose
// one. Two distinct bad states have to stay distinguishable here: "nothing is
// being injected, as configured" and "a sink that should inject cannot" --
// the second is the Wayland trap, and a startup line that blurred them would
// be the trap's accomplice.
std::string SinkStatus(const hc::CliOptions& opts,
                       const hc::Controller& controller) {
  const hc::SinkKind kind = hc::ResolveSink(opts);
  const std::string name = hc::SinkKindName(kind);

  if (!controller.SinkAvailable()) return name + " (UNAVAILABLE)";
  if (kind == hc::SinkKind::Uinput || kind == hc::SinkKind::XTest ||
      kind == hc::SinkKind::SendInput) {
    return name + ", injecting input";
  }
  return name + ", no input injection";
}

// A one-line summary of the run, drawn into the overlay so what is on screen
// says what produced it.
std::string BuildHud(const hc::CliOptions& opts, std::uint64_t frame,
                     std::size_t hands, std::uint64_t dropped,
                     const std::string& sink_status) {
  return "frame " + std::to_string(frame) + "  hands " + std::to_string(hands) +
         "/" + std::to_string(opts.max_hands) + "  dropped " +
         std::to_string(dropped) + "  sink " + sink_status;
}

}  // namespace

int main(int argc, char** argv) {
  hc::CliOptions opts;
  std::string error;
  if (!hc::ParseCli(argc, argv, opts, error)) {
    std::cerr << "error: " << error << "\n\n" << hc::CliUsage();
    return 2;
  }
  if (opts.want_help) {
    std::cout << hc::CliUsage();
    return 0;
  }

  hc::ControllerConfig controller_config;
  if (!LoadBindings(opts, controller_config.bindings)) return 1;
  controller_config.frame_limit = opts.frames;

  std::unique_ptr<hc::IOutputSink> sink = MakeSink(opts);
  if (!sink) return 1;

  std::unique_ptr<hc::IFrameSource> source = MakeSource(opts);
  if (!source) return 1;

  // Wrapped so capture runs on its own thread and the loop below always gets
  // the newest frame rather than a backlog of stale ones. The dropped count is
  // part of the measurement, not an error. Held as a raw pointer as well
  // because dropped() is on the concrete type and the Controller owns it.
  auto latest = std::make_unique<hc::LatestFrameSource>(std::move(source));
  hc::LatestFrameSource* capture = latest.get();

  hc::Metrics metrics;

  hc::HandTrackerConfig config;
  config.palm_model_path = HC_PALM_MODEL_PATH;
  config.landmark_model_path = HC_LANDMARK_MODEL_PATH;
  config.max_hands = opts.max_hands;
  config.intra_op_threads = opts.threads;

  // The tracker loads both models in its constructor and throws if either is
  // missing or has an unexpected layout. That is a startup failure with a
  // useful message, not a crash.
  std::unique_ptr<hc::IHandTracker> tracker;
  try {
    tracker = std::make_unique<hc::HandTrackerOnnx>(config, metrics);
  } catch (const std::exception& e) {
    std::cerr << "error: could not initialise the tracker: " << e.what() << "\n";
    return 1;
  }

  hc::Controller controller(std::move(latest), std::move(tracker),
                            std::move(sink), std::move(controller_config),
                            &metrics);
  if (!controller.Start()) {
    std::cerr << "error: could not start the session\n";
    return 1;
  }

  // Said once, at startup, because "it is running" and "it is actually doing
  // something" are different claims and the Wayland probe is the reason to be
  // careful about which one is being made.
  const std::string sink_status = SinkStatus(opts, controller);
  std::cout << "sink: " << sink_status << "\n";

  std::unique_ptr<hc::Overlay> overlay;
  if (opts.want_overlay) overlay = std::make_unique<hc::Overlay>("hand_controller");

  // Held across the loop and refilled by the poll calls, so the steady state
  // does not allocate.
  std::vector<hc::HandObservation> observations;
  std::vector<hc::GestureEvent> events;
  std::vector<hc::HandLabel> labels;

  // Wall-clock span of the whole loop. Reported alongside the per-stage table
  // because it is the only number here that includes everything -- capture
  // waits, inference, drawing and loop overhead together -- and so is the one
  // the achieved frame rate has to be derived from.
  const auto run_start = std::chrono::high_resolution_clock::now();

  while (controller.Step()) {
    if (!overlay) continue;

    controller.PollObservations(observations);

    // One caption per gesture event, which is at most one per hand per frame.
    // The words come from hc_control: the overlay places text, it does not
    // decide what a pose is called.
    controller.PollEvents(events);
    labels.clear();
    for (const hc::GestureEvent& e : events) {
      labels.push_back(
          hc::HandLabel{e.track_id, hc::DescribeGesture(e.kind, e.phase)});
    }

    overlay->Render(controller.LastFrame().view, observations, labels,
                    BuildHud(opts, controller.ProcessedFrames(),
                             observations.size(), capture->dropped(),
                             sink_status));
    if (overlay->ShouldQuit()) break;
  }

  const auto run_end = std::chrono::high_resolution_clock::now();

  // EVERY exit path arrives here: the frame budget, the source ending, and
  // the overlay's quit key. Review focus 7 -- a key still down outlives the
  // process, so the release cannot live on only one of those paths. The
  // destructor would also do it; this is the explicit one so the ordering
  // against the report below is visible rather than incidental.
  controller.Stop();

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(run_end - run_start).count();
  const std::uint64_t processed = controller.ProcessedFrames();

  // Built once and written to both sinks, so the file and the terminal can
  // never disagree about what the run measured. Format() does not end in a
  // newline, hence the explicit one.
  std::ostringstream summary;
  summary << metrics.Format() << "\n"
          << "frames processed: " << processed << "\n"
          << "frames dropped:   " << capture->dropped() << "\n";
  summary << std::fixed << std::setprecision(3)
          << "wall clock:       " << elapsed_ms << " ms\n";
  if (processed > 0 && elapsed_ms > 0.0) {
    summary << "per frame:        " << elapsed_ms / static_cast<double>(processed)
            << " ms\n"
            << "frame rate:       "
            << 1000.0 * static_cast<double>(processed) / elapsed_ms << " fps\n";
  }

  std::cout << summary.str();

  if (!opts.report_path.empty()) {
    std::ofstream file(opts.report_path);
    if (!file) {
      std::cerr << "error: could not write the report to " << opts.report_path
                << "\n";
      return 1;
    }
    file << summary.str();
  }

  return 0;
}
