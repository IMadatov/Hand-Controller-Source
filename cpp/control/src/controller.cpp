#include "hc/controller.hpp"

#include <chrono>
#include <utility>

namespace hc {

Controller::Controller(std::unique_ptr<IFrameSource> source,
                       std::unique_ptr<IHandTracker> tracker,
                       std::unique_ptr<IOutputSink> sink,
                       ControllerConfig config, Metrics* metrics)
    : source_(std::move(source)),
      tracker_(std::move(tracker)),
      sink_(std::move(sink)),
      metrics_(metrics),
      recognizer_(config.gestures),
      mapper_(std::move(config.bindings)),
      frame_limit_(config.frame_limit) {}

Controller::~Controller() { Stop(); }

bool Controller::Start() {
  if (state_ != State::Idle) return false;
  if (source_ == nullptr || tracker_ == nullptr) return false;
  state_ = State::Running;
  return true;
}

void Controller::Stop() {
  if (state_ == State::Stopped) return;

  // Releases FIRST, then close. A key is released through the sink, and a
  // QueuedSink's ReleaseAll blocks until its worker has drained -- doing this
  // after tearing anything down would be the one ordering where a key can
  // stay pressed.
  if (sink_ != nullptr) sink_->ReleaseAll();
  if (source_ != nullptr) source_->Close();

  state_ = State::Stopped;
}

bool Controller::Step() {
  if (state_ != State::Running) return false;

  if (frame_limit_ > 0 &&
      processed_ >= static_cast<std::uint64_t>(frame_limit_)) {
    state_ = State::Ended;
    return false;
  }

  // Timed separately because it is time spent waiting on a camera, which is
  // not a cost this code can optimise; folded into any other stage it would
  // distort every number in the report.
  const auto wait_start = std::chrono::high_resolution_clock::now();
  const bool got = source_->Next(frame_);
  const auto wait_end = std::chrono::high_resolution_clock::now();
  if (metrics_ != nullptr) {
    metrics_->Add(Stage::CaptureWait,
                  std::chrono::duration<double, std::milli>(wait_end -
                                                            wait_start)
                      .count());
  }

  if (!got) {
    // Terminal, per IFrameSource. The session is over, but it is not STOPPED:
    // the caller still has its own shutdown to run, and Stop() is its call.
    state_ = State::Ended;
    return false;
  }

  // Not timed as Stage::Total: the tracker records that stage itself, and a
  // second sample per frame would push two different quantities into one
  // histogram and silently corrupt its mean and percentiles.
  tracker_->Process(frame_.view, frame_.timestamp_us, observations_);

  // The timestamp is the FRAME's, passed straight through. Neither of these
  // layers reads a clock, which is what lets the whole chain be replayed from
  // a script in a test.
  recognizer_.Process(observations_, frame_.timestamp_us, events_);
  mapper_.Map(events_, commands_);

  for (const Command& c : commands_) {
    if (sink_ != nullptr) sink_->Send(c);
  }

  ++processed_;
  return true;
}

void Controller::PollObservations(std::vector<HandObservation>& out) const {
  out = observations_;
}

void Controller::PollCommands(std::vector<Command>& out) const {
  out = commands_;
}

void Controller::PollEvents(std::vector<GestureEvent>& out) const {
  out = events_;
}

void Controller::SetBindings(BindingSet bindings) {
  mapper_.SetBindings(std::move(bindings));
}

bool Controller::SinkAvailable() const {
  return sink_ != nullptr && sink_->Available();
}

}  // namespace hc
