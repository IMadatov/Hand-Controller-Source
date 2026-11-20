#include "hc/controller.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hand_poses.hpp"
#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/hand_observation.hpp"
#include "hc/logging_sink.hpp"
#include "hc/synthetic_frame_source.hpp"

using hc::BindingSet;
using hc::Command;
using hc::Controller;
using hc::ControllerConfig;
using hc::Handedness;
using hc::HandObservation;
using hc::LoggingSink;
using hc::SyntheticFrameSource;

namespace {

// A tracker that replays a script instead of looking at the pixels.
//
// The point of the Controller tests is the WIRING -- source to tracker to
// recognizer to mapper to sink, and the release on every exit path. Running
// the real ONNX tracker here would make them depend on two model files and
// on inference output nobody scripted, and a failure would not say which
// layer broke. The real tracker has its own tests.
class ScriptedTracker : public hc::IHandTracker {
 public:
  explicit ScriptedTracker(std::vector<std::vector<HandObservation>> script)
      : script_(std::move(script)) {}

  void Process(const hc::ImageView&, std::int64_t timestamp_us,
               std::vector<HandObservation>& out) override {
    out.clear();
    ++calls_;
    if (script_.empty()) return;

    // The last entry repeats, so a script shorter than the run does not
    // suddenly drop every hand and end every gesture.
    const std::size_t i =
        calls_ - 1 < script_.size() ? calls_ - 1 : script_.size() - 1;
    out = script_[i];
    for (HandObservation& o : out) o.timestamp_us = timestamp_us;
  }

  void Reset() override { calls_ = 0; }

  std::size_t calls() const { return calls_; }

 private:
  std::vector<std::vector<HandObservation>> script_;
  std::size_t calls_ = 0;
};

// Forwards to a sink the test owns, so the recording outlives the controller
// that was handed the sink. The controller takes ownership of what it is
// given, which is right for production and inconvenient for exactly one
// assertion: that the DESTRUCTOR releases.
class BorrowedSink : public hc::IOutputSink {
 public:
  explicit BorrowedSink(hc::IOutputSink* inner) : inner_(inner) {}
  void Send(const Command& command) override { inner_->Send(command); }
  void ReleaseAll() override { inner_->ReleaseAll(); }
  bool Available() const override { return inner_->Available(); }

 private:
  hc::IOutputSink* inner_;
};

std::unique_ptr<hc::IFrameSource> Frames(int count) {
  // Small frames: nothing here reads a pixel, and a 64x48 synthetic source
  // keeps the test's memory traffic irrelevant to its runtime.
  return std::make_unique<SyntheticFrameSource>(64, 48, count);
}

BindingSet ShippedBindings() {
  BindingSet set;
  std::string error;
  EXPECT_TRUE(hc::ParseBindings(hc::DefaultBindingsText(), set, error))
      << error;
  return set;
}

ControllerConfig ShippedConfig() {
  ControllerConfig config;
  config.bindings = ShippedBindings();
  return config;
}

// A right hand pointing and drifting rightward: the shipped configuration's
// cursor movement, which is `held`, so every frame after the first produces a
// command.
std::vector<std::vector<HandObservation>> PointingDrift(int frames) {
  std::vector<std::vector<HandObservation>> script;
  for (int i = 0; i < frames; ++i) {
    const float x = 0.45f + static_cast<float>(i) * 0.01f;
    script.push_back({hc_test::PointIndex(1, Handedness::Right, {x, 0.8f})});
  }
  return script;
}

int CountKind(const std::vector<Command>& commands, Command::Kind kind) {
  int n = 0;
  for (const Command& c : commands) {
    if (c.kind == kind) ++n;
  }
  return n;
}

// Runs the whole session and returns the number of frames stepped.
int RunToEnd(Controller& controller) {
  int steps = 0;
  while (controller.Step()) ++steps;
  return steps;
}

}  // namespace

TEST(Controller, StepRunsThePipelineAndDeliversCommandsToTheSink) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  Controller controller(Frames(10),
                        std::make_unique<ScriptedTracker>(PointingDrift(10)),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  EXPECT_EQ(RunToEnd(controller), 10);
  controller.Stop();

  // `began` carries no motion by contract, so the first pointing frame moves
  // nothing and the nine after it each move the cursor.
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::MoveCursorRelative),
            9);
}

TEST(Controller, PollReportsTheLastFrameOnly) {
  Controller controller(Frames(5),
                        std::make_unique<ScriptedTracker>(PointingDrift(5)),
                        /*sink=*/nullptr, ShippedConfig());
  ASSERT_TRUE(controller.Start());

  std::vector<HandObservation> hands;
  std::vector<Command> commands;

  ASSERT_TRUE(controller.Step());
  controller.PollObservations(hands);
  controller.PollCommands(commands);
  EXPECT_EQ(hands.size(), 1u);
  EXPECT_TRUE(commands.empty()) << "the first frame is a Began, which the "
                                   "shipped config binds on held only";

  ASSERT_TRUE(controller.Step());
  controller.PollObservations(hands);
  controller.PollCommands(commands);
  EXPECT_EQ(hands.size(), 1u);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].kind, Command::Kind::MoveCursorRelative);

  // Polling twice without stepping reports the same frame rather than
  // draining a queue: these are the last frame's results, not a backlog.
  std::vector<Command> again;
  controller.PollCommands(again);
  EXPECT_EQ(again.size(), commands.size());
}

// The GUI and a server both need this: a session that recognizes gestures and
// reports commands as DATA without touching the machine's input.
TEST(Controller, WithNoSinkIsFullyUsableAndReportsNoSink) {
  Controller controller(Frames(6),
                        std::make_unique<ScriptedTracker>(PointingDrift(6)),
                        /*sink=*/nullptr, ShippedConfig());
  ASSERT_TRUE(controller.Start());
  EXPECT_FALSE(controller.SinkAvailable());
  EXPECT_EQ(RunToEnd(controller), 6);

  std::vector<Command> commands;
  controller.PollCommands(commands);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].kind, Command::Kind::MoveCursorRelative);
  controller.Stop();
}

// Review focus 7. The left thumb_up binding presses arrow_right on `began`
// and releases it on `ended`; a session stopped mid-gesture never reaches the
// `ended`, so the release has to come from the shutdown path.
TEST(Controller, StopReleasesAKeyStillHeldWhenTheSessionEnds) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  std::vector<std::vector<HandObservation>> script;
  script.push_back({hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});

  Controller controller(Frames(4), std::make_unique<ScriptedTracker>(script),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  EXPECT_EQ(RunToEnd(controller), 4);

  // Still held: the hand never left and the pose never relaxed.
  ASSERT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyPress), 1);
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyRelease), 0);

  controller.Stop();
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyRelease), 1);
  EXPECT_EQ(recorded->Commands().back().key, hc::Key::ArrowRight);
}

// The path that runs even when the code above forgot, and the one that runs
// on an exception unwinding out of the loop.
TEST(Controller, DestructionReleasesWithoutAnExplicitStop) {
  LoggingSink recorded;
  {
    std::vector<std::vector<HandObservation>> script;
    script.push_back({hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});

    // Borrowed rather than owned, so the recording outlives the controller.
    Controller controller(
        Frames(3), std::make_unique<ScriptedTracker>(script),
        std::make_unique<BorrowedSink>(&recorded),
        ShippedConfig());
    ASSERT_TRUE(controller.Start());
    RunToEnd(controller);
    ASSERT_EQ(CountKind(recorded.Commands(), Command::Kind::KeyPress), 1);
  }
  EXPECT_EQ(CountKind(recorded.Commands(), Command::Kind::KeyRelease), 1);
}

TEST(Controller, StepReturnsFalseWhenTheSourceEnds) {
  Controller controller(Frames(3),
                        std::make_unique<ScriptedTracker>(PointingDrift(3)),
                        /*sink=*/nullptr, ShippedConfig());
  ASSERT_TRUE(controller.Start());
  EXPECT_TRUE(controller.Step());
  EXPECT_TRUE(controller.Step());
  EXPECT_TRUE(controller.Step());
  EXPECT_FALSE(controller.Step());
  // And stays false: a terminal source is terminal, per IFrameSource.
  EXPECT_FALSE(controller.Step());
  EXPECT_EQ(controller.ProcessedFrames(), 3u);
}

TEST(Controller, HonoursTheFrameLimit) {
  ControllerConfig config = ShippedConfig();
  config.frame_limit = 4;

  Controller controller(Frames(100),
                        std::make_unique<ScriptedTracker>(PointingDrift(100)),
                        /*sink=*/nullptr, config);
  ASSERT_TRUE(controller.Start());
  EXPECT_EQ(RunToEnd(controller), 4);
  EXPECT_EQ(controller.ProcessedFrames(), 4u);
}

TEST(Controller, StepBeforeStartDoesNothing) {
  Controller controller(Frames(5),
                        std::make_unique<ScriptedTracker>(PointingDrift(5)),
                        /*sink=*/nullptr, ShippedConfig());
  EXPECT_FALSE(controller.Step());
  EXPECT_EQ(controller.ProcessedFrames(), 0u);
}

TEST(Controller, StopIsIdempotentAndSafeBeforeStart) {
  Controller controller(Frames(5),
                        std::make_unique<ScriptedTracker>(PointingDrift(5)),
                        /*sink=*/nullptr, ShippedConfig());
  controller.Stop();
  controller.Stop();
  // And a stopped session does not restart: Start after Stop is refused
  // rather than silently reopening a closed source.
  EXPECT_FALSE(controller.Start());
}

TEST(Controller, StartTwiceIsRefused) {
  Controller controller(Frames(5),
                        std::make_unique<ScriptedTracker>(PointingDrift(5)),
                        /*sink=*/nullptr, ShippedConfig());
  EXPECT_TRUE(controller.Start());
  EXPECT_FALSE(controller.Start());
}

// Bindings are editable at runtime -- that is the whole point of the bindings
// UI -- so the controller must take a new set without being rebuilt.
TEST(Controller, SetBindingsTakesEffectOnTheNextFrame) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  Controller controller(Frames(8),
                        std::make_unique<ScriptedTracker>(PointingDrift(8)),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(controller.Step());
  const int moved = CountKind(recorded->Commands(),
                              Command::Kind::MoveCursorRelative);
  ASSERT_GT(moved, 0);

  // An empty set binds nothing, which is a valid configuration.
  controller.SetBindings(BindingSet{});
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(controller.Step());
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::MoveCursorRelative),
            moved);
  controller.Stop();
}

// One session per Controller, no shared state: two controllers running the
// same script must not see each other's gesture latches or smoothing.
TEST(Controller, TwoControllersDoNotShareState) {
  auto sink_a = std::make_unique<LoggingSink>();
  auto sink_b = std::make_unique<LoggingSink>();
  const LoggingSink* a = sink_a.get();
  const LoggingSink* b = sink_b.get();

  Controller one(Frames(6),
                 std::make_unique<ScriptedTracker>(PointingDrift(6)),
                 std::move(sink_a), ShippedConfig());
  Controller two(Frames(6),
                 std::make_unique<ScriptedTracker>(PointingDrift(6)),
                 std::move(sink_b), ShippedConfig());
  ASSERT_TRUE(one.Start());
  ASSERT_TRUE(two.Start());

  // Interleaved on purpose: shared recognizer state would show up as the
  // second controller's Began being swallowed by the first's latch.
  while (one.Step() && two.Step()) {
  }
  one.Stop();
  two.Stop();

  EXPECT_EQ(CountKind(a->Commands(), Command::Kind::MoveCursorRelative),
            CountKind(b->Commands(), Command::Kind::MoveCursorRelative));
  EXPECT_GT(CountKind(a->Commands(), Command::Kind::MoveCursorRelative), 0);
}

// --- phase 3: what the GUI's engine interface needs ----------------------

// "Keep the camera, stop emitting commands" -- the GUI's paused state. The
// session stays live and the user keeps seeing their hand.
TEST(Controller, PausedStillReportsCommandsButSendsNone) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  Controller controller(Frames(10),
                        std::make_unique<ScriptedTracker>(PointingDrift(10)),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(controller.Step());
  const int before = CountKind(recorded->Commands(),
                               Command::Kind::MoveCursorRelative);
  ASSERT_GT(before, 0);

  controller.SetPaused(true);
  EXPECT_TRUE(controller.Paused());

  std::vector<Command> commands;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(controller.Step());
    controller.PollCommands(commands);
    EXPECT_EQ(commands.size(), 1u) << "a paused session still produces data";
  }
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::MoveCursorRelative),
            before)
      << "but the sink heard nothing";

  controller.SetPaused(false);
  ASSERT_TRUE(controller.Step());
  EXPECT_GT(CountKind(recorded->Commands(), Command::Kind::MoveCursorRelative),
            before);
  controller.Stop();
}

// Review focus 7, at the moment it is easiest to forget: pausing while a
// gesture holds a key down must not leave it down for the pause.
TEST(Controller, PausingReleasesAHeldKey) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  std::vector<std::vector<HandObservation>> script;
  script.push_back({hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});

  Controller controller(Frames(8), std::make_unique<ScriptedTracker>(script),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(controller.Step());
  ASSERT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyPress), 1);
  ASSERT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyRelease), 0);

  controller.SetPaused(true);
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyRelease), 1);
}

TEST(Controller, PausingTwiceReleasesOnlyOnce) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  std::vector<std::vector<HandObservation>> script;
  script.push_back({hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});

  Controller controller(Frames(8), std::make_unique<ScriptedTracker>(script),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  ASSERT_TRUE(controller.Step());
  controller.SetPaused(true);
  controller.SetPaused(true);
  EXPECT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyRelease), 1);
}

// The GUI toggles injection while running. Rebuilding the session for it would
// drop the tracker, the ROI and every gesture latch -- the user's hand would
// visibly flicker for having ticked a checkbox.
TEST(Controller, SetSinkSwapsWhereCommandsGoWithoutRestarting) {
  // Test-owned log: SetSink destroys the outgoing sink, and `one` is read
  // afterwards, so the recording must outlive the BorrowedSink handed over.
  LoggingSink log_one;
  const LoggingSink* one = &log_one;
  auto first = std::make_unique<BorrowedSink>(&log_one);

  Controller controller(Frames(12),
                        std::make_unique<ScriptedTracker>(PointingDrift(12)),
                        std::move(first), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(controller.Step());
  const int on_first = CountKind(one->Commands(),
                                 Command::Kind::MoveCursorRelative);
  ASSERT_GT(on_first, 0);

  auto second = std::make_unique<LoggingSink>();
  const LoggingSink* two = second.get();
  controller.SetSink(std::move(second));

  for (int i = 0; i < 4; ++i) ASSERT_TRUE(controller.Step());
  EXPECT_EQ(CountKind(one->Commands(), Command::Kind::MoveCursorRelative),
            on_first)
      << "the old sink must hear nothing more";
  EXPECT_GT(CountKind(two->Commands(), Command::Kind::MoveCursorRelative), 0);

  // And the gesture was never interrupted: no Began after the first frame,
  // which is what proves the session was not rebuilt underneath.
  std::vector<hc::GestureEvent> events;
  controller.PollEvents(events);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].phase, hc::GestureEvent::Phase::Held);
  controller.Stop();
}

// The outgoing sink is the only thing that can release what it pressed, and a
// moment later it does not exist. Switching injection off mid-gesture is
// exactly when a key gets stranded.
TEST(Controller, SetSinkReleasesTheOutgoingSinksHeldKeys) {
  // Test-owned log: the outgoing sink is destroyed inside SetSink and `one`
  // is read afterwards.
  LoggingSink log_one;
  const LoggingSink* one = &log_one;
  auto first = std::make_unique<BorrowedSink>(&log_one);

  std::vector<std::vector<HandObservation>> script;
  script.push_back({hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});

  Controller controller(Frames(8), std::make_unique<ScriptedTracker>(script),
                        std::move(first), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 2; ++i) ASSERT_TRUE(controller.Step());
  ASSERT_EQ(CountKind(one->Commands(), Command::Kind::KeyPress), 1);

  controller.SetSink(nullptr);  // Injection off entirely.
  EXPECT_EQ(CountKind(one->Commands(), Command::Kind::KeyRelease), 1);
  EXPECT_FALSE(controller.SinkAvailable());

  // And the session carries on, which is preview mode. Asserted on EVENTS,
  // not commands: the shipped configuration binds thumb_up on `began` and
  // `ended` only, so a held thumb-up legitimately produces no command at all.
  // Checking commands here would have been testing the bindings file.
  ASSERT_TRUE(controller.Step());
  std::vector<hc::GestureEvent> events;
  controller.PollEvents(events);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, hc::GestureKind::ThumbUp);
  EXPECT_EQ(events[0].phase, hc::GestureEvent::Phase::Held);
}

// A command log has to say which hand and which gesture caused each entry.
// The Command itself carries none of that on purpose.
TEST(Controller, CommandOriginsPointAtTheEventThatCausedEach) {
  std::vector<std::vector<HandObservation>> script;
  // Right hand pointing (move_cursor on held) and left thumb-up (key_press
  // on began) in the same frame, so the two origins must not be confusable.
  for (int i = 0; i < 6; ++i) {
    const float x = 0.45f + static_cast<float>(i) * 0.01f;
    script.push_back({hc_test::PointIndex(1, Handedness::Right, {x, 0.8f}),
                      hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f})});
  }

  Controller controller(Frames(6), std::make_unique<ScriptedTracker>(script),
                        /*sink=*/nullptr, ShippedConfig());
  ASSERT_TRUE(controller.Start());
  ASSERT_TRUE(controller.Step());  // Both hands Began.

  std::vector<hc::GestureEvent> events;
  std::vector<Command> commands;
  std::vector<std::uint32_t> origins;
  controller.PollEvents(events);
  controller.PollCommands(commands);
  controller.PollCommandOrigins(origins);

  ASSERT_EQ(origins.size(), commands.size());
  ASSERT_FALSE(commands.empty());
  for (std::size_t i = 0; i < commands.size(); ++i) {
    ASSERT_LT(origins[i], events.size());
    const hc::GestureEvent& cause = events[origins[i]];
    if (commands[i].kind == Command::Kind::KeyPress) {
      EXPECT_EQ(cause.handedness, Handedness::Left);
      EXPECT_EQ(cause.kind, hc::GestureKind::ThumbUp);
      EXPECT_EQ(cause.phase, hc::GestureEvent::Phase::Began);
    }
  }
}

TEST(Controller, CommandOriginsAreEmptyWhenNothingMatched) {
  Controller controller(Frames(4),
                        std::make_unique<ScriptedTracker>(PointingDrift(4)),
                        /*sink=*/nullptr, ControllerConfig{});  // No bindings.
  ASSERT_TRUE(controller.Start());
  ASSERT_TRUE(controller.Step());

  std::vector<std::uint32_t> origins;
  controller.PollCommandOrigins(origins);
  EXPECT_TRUE(origins.empty());
}

// --- one physical gesture must not fire twice across a tracking dropout -----

namespace {

using Frame = std::vector<HandObservation>;

void AddFrames(std::vector<Frame>& script, int count,
               const HandObservation& hand) {
  for (int i = 0; i < count; ++i) script.push_back({hand});
}

// Runs the script to its end and returns what the sink heard.
std::vector<Command> RunScript(std::vector<Frame> script) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();
  const int frames = static_cast<int>(script.size());
  Controller controller(Frames(frames),
                        std::make_unique<ScriptedTracker>(std::move(script)),
                        std::move(sink), ShippedConfig());
  EXPECT_TRUE(controller.Start());
  RunToEnd(controller);
  std::vector<Command> heard = recorded->Commands();  // Before Stop().
  controller.Stop();
  return heard;
}

int Clicks(const std::vector<Command>& c) {
  return CountKind(c, Command::Kind::Click);
}

}  // namespace

TEST(Controller, DropoutAndNewTrackIdDoesNotRefireClick) {
  std::vector<Frame> script;
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  script.push_back({});  // One frame of presence dropout.
  AddFrames(script, 5,
            hc_test::TwoFingers(2, Handedness::Right, {0.5f, 0.8f}));
  EXPECT_EQ(Clicks(RunScript(script)), 1);
}

TEST(Controller, DropoutWithSameTrackIdDoesNotRefireClick) {
  std::vector<Frame> script;
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  script.push_back({});
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  EXPECT_EQ(Clicks(RunScript(script)), 1);
}

// Guard: a fix must not suppress genuine repeats.
TEST(Controller, ClickAfterLongAbsenceFiresAgain) {
  std::vector<Frame> script;
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  for (int i = 0; i < 15; ++i) script.push_back({});
  AddFrames(script, 5,
            hc_test::TwoFingers(2, Handedness::Right, {0.5f, 0.8f}));
  EXPECT_EQ(Clicks(RunScript(script)), 2);
}

// Guard: pose released (hand still visible), then made again.
TEST(Controller, ClickAfterPoseReleasedFiresAgain) {
  std::vector<Frame> script;
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  AddFrames(script, 12, hc_test::OpenPalm(1, Handedness::Right, {0.5f, 0.8f}));
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.5f, 0.8f}));
  EXPECT_EQ(Clicks(RunScript(script)), 2);
}

// Guard: a hand reappearing far away after a one-frame gap is a different
// gesture (more than two palm-sizes, ~0.6 of the frame here).
TEST(Controller, ClickReappearingFarAwayFiresAgain) {
  std::vector<Frame> script;
  AddFrames(script, 5,
            hc_test::TwoFingers(1, Handedness::Right, {0.2f, 0.8f}));
  script.push_back({});
  AddFrames(script, 5,
            hc_test::TwoFingers(2, Handedness::Right, {0.9f, 0.8f}));
  EXPECT_EQ(Clicks(RunScript(script)), 2);
}

// Safety guard: a key pressed by a gesture is released when the hand leaves.
TEST(Controller, KeyReleasedWithinFiveFramesOfHandLeaving) {
  auto sink = std::make_unique<LoggingSink>();
  const LoggingSink* recorded = sink.get();

  std::vector<Frame> script;
  AddFrames(script, 4, hc_test::ThumbUp(2, Handedness::Left, {0.2f, 0.8f}));
  for (int i = 0; i < 10; ++i) script.push_back({});

  Controller controller(Frames(14),
                        std::make_unique<ScriptedTracker>(script),
                        std::move(sink), ShippedConfig());
  ASSERT_TRUE(controller.Start());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(controller.Step());
  ASSERT_EQ(CountKind(recorded->Commands(), Command::Kind::KeyPress), 1);
  ASSERT_EQ(recorded->Commands().back().key, hc::Key::ArrowRight);

  bool released = false;
  for (int i = 0; i < 5 && !released; ++i) {
    ASSERT_TRUE(controller.Step());
    released = CountKind(recorded->Commands(), Command::Kind::KeyRelease) == 1;
  }
  EXPECT_TRUE(released) << "no key_release within 5 frames of the hand leaving"
                           ", before Stop()";
  controller.Stop();
}
