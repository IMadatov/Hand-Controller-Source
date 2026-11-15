#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "hc/action_mapper.hpp"
#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/frame_source.hpp"
#include "hc/gesture_event.hpp"
#include "hc/gesture_recognizer.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/metrics.hpp"
#include "hc/output_sink.hpp"

namespace hc {

// What a session is configured with. Everything here is plain data, so a
// frontend can build one from a command line, a GUI, or a C ABI call without
// any of them learning about the layers below.
struct ControllerConfig {
  // What the gestures mean. Default-constructed binds nothing, which is a
  // valid configuration and not an error state.
  BindingSet bindings;

  // Recognizer tuning. Separate from `bindings` because it is not in the
  // bindings file format: thresholds are the recognizer's calibration, not
  // the user's choice of what a pose does.
  GestureConfig gestures = GestureConfig::Default();

  // Stop after this many frames. 0 means run until the source ends or Stop()
  // is called -- the normal interactive case.
  int frame_limit = 0;
};

// One session: a frame source, a tracker, gesture recognition, mapping, and
// an optional sink, driven one frame at a time by whoever owns the loop.
//
// WHY THIS CLASS EXISTS rather than a loop in main(): a GUI has its own event
// loop, a .NET host has its own process model, and a server runs many of these
// at once. If the loop lived in main(), each of those would reimplement it and
// diverge from it within a release. `main()` is one frontend over this, not
// the only possible one.
//
// Three guarantees, each from a scenario the owner raised:
//
//   1. ONE INSTANCE PER SESSION. No singleton, no global state, no static
//      anything. Per-hand ROI, gesture latches and smoothing are all
//      per-session, so two Controllers in one process must not be able to
//      see each other's.
//
//   2. A CONTROLLER WITH NO SINK IS FULLY USABLE, and reports its commands as
//      data through PollCommands. That is the only sane mode for a server (it
//      would otherwise move the SERVER's cursor), for a browser client, and
//      for the GUI's preview mode. It is a supported configuration, not a
//      degraded one.
//
//   3. STOP RELEASES EVERYTHING. A key still down outlives the process, and
//      the destructor runs on paths the code above forgot, including an
//      exception unwinding out of the caller's loop.
//
// NOT THREAD-SAFE, and deliberately so: the frame sources it drives are not
// either (LatestFrameSource is the one exception and keeps its own rules).
// Drive one Controller from one thread. Two sessions means two Controllers,
// which is the point of guarantee 1.
class Controller {
 public:
  // Takes ownership of all three. `sink` MAY BE NULL -- see guarantee 2.
  //
  // `metrics` is borrowed and may be null. When present the controller adds
  // its own Stage::CaptureWait samples to it; the tracker records its own
  // stages directly and this class does not double-count them.
  Controller(std::unique_ptr<IFrameSource> source,
             std::unique_ptr<IHandTracker> tracker,
             std::unique_ptr<IOutputSink> sink, ControllerConfig config,
             Metrics* metrics = nullptr);

  // Stops the session if it is still running, which releases every held key
  // and button. See guarantee 3.
  ~Controller();

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  // Begins the session. Returns false if it is already running, has already
  // been stopped, or was built without a source or a tracker.
  //
  // A stopped session does not restart: the source has been closed and
  // IFrameSource makes that terminal. Build a new Controller.
  bool Start();

  // Advances exactly one frame: pull, track, recognize, map, dispatch.
  //
  // Returns false when the session is over -- the source ended, the frame
  // limit was reached, Stop() was called, or Start() never was. Once it has
  // returned false it returns false forever, mirroring IFrameSource.
  //
  // Does NOT call Stop() on its own. The caller decides when the session
  // ends, because a frontend usually has shutdown work of its own to do first
  // (a report to print, a window to close) and a Stop buried in here would
  // make the ordering of that invisible.
  bool Step();

  // Ends the session: releases everything the sink holds, then closes the
  // source. Idempotent, and safe before Start() -- both matter because this
  // runs on abnormal exit paths where "already stopped" is normal.
  void Stop();

  // The hands seen on the LAST stepped frame, copied into `out`, which is
  // cleared first. Not a queue: polling twice without stepping reports the
  // same frame again.
  //
  // Caller-owned buffer, so a loop can hold one across the session and stop
  // allocating once it has grown. Same convention as IHandTracker::Process.
  void PollObservations(std::vector<HandObservation>& out) const;

  // The commands the last stepped frame produced, on the same terms.
  //
  // These are reported whether or not a sink was attached, and they are the
  // same values the sink received. A frontend rendering "what the gesture
  // did" must read them here rather than instrumenting the sink, or it would
  // show nothing in the no-sink configuration.
  void PollCommands(std::vector<Command>& out) const;

  // The gesture events behind those commands, for a UI that shows the gesture
  // itself rather than only its effect. An event with no binding produces no
  // command but still appears here.
  void PollEvents(std::vector<GestureEvent>& out) const;

  // The last stepped frame, for a frontend that draws it.
  //
  // LIFETIME: `view` is non-owning and belongs to the frame source. It is
  // valid only until the next Step() or Stop(). A consumer that needs the
  // pixels beyond that -- anything crossing a thread or a language boundary
  // -- must copy them. This is IFrameSource's own contract, passed through
  // unchanged rather than papered over with a copy nobody asked for.
  const Frame& LastFrame() const { return frame_; }

  // Replaces the bindings mid-session, dropping smoothing state with them.
  // The bindings UI exists to edit bindings and apply them without a restart,
  // which is the only reason this is not construction-only.
  void SetBindings(BindingSet bindings);

  const BindingSet& Bindings() const { return mapper_.Bindings(); }

  // True when a sink is attached AND it reports itself able to deliver.
  //
  // Both halves matter. No sink is a deliberate configuration; an attached
  // sink that cannot work -- XTest under Wayland, which returns success and
  // moves nothing -- is a fault the frontend must be able to SAY, and this is
  // what it asks.
  //
  // NOT "is it injecting input". Delivering is not injecting: a logging sink
  // is perfectly available and moves no cursor. Which sinks inject is the
  // frontend's own knowledge, because the frontend is what chose one -- this
  // layer only knows whether what it was handed works.
  bool SinkAvailable() const;

  std::uint64_t ProcessedFrames() const { return processed_; }

 private:
  // `Ended` and `Stopped` are deliberately different states. A source that
  // runs out ends the SESSION; it does not perform the SHUTDOWN, because the
  // frontend still has its own work to do first and Stop() is its call. If
  // these were one state, Stop() after a source ended would look redundant
  // and skip the release -- which is a key left down.
  enum class State { Idle, Running, Ended, Stopped };

  std::unique_ptr<IFrameSource> source_;
  std::unique_ptr<IHandTracker> tracker_;
  std::unique_ptr<IOutputSink> sink_;
  Metrics* metrics_;

  GestureRecognizer recognizer_;
  ActionMapper mapper_;

  State state_ = State::Idle;
  int frame_limit_;
  std::uint64_t processed_ = 0;

  // The last frame's results, held so Poll* can report them and so the steady
  // state allocates nothing.
  Frame frame_{};
  std::vector<HandObservation> observations_;
  std::vector<GestureEvent> events_;
  std::vector<Command> commands_;
};

}  // namespace hc
