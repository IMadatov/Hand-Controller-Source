#include "hc/action_mapper.hpp"

#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/hand_observation.hpp"

using hc::ActionMapper;
using hc::BindingSet;
using hc::Command;
using hc::GestureEvent;
using hc::GestureKind;
using hc::Handedness;
using hc::Key;
using hc::MouseButton;
using hc::ParseBindings;
using hc::Point2;
using Phase = hc::GestureEvent::Phase;

namespace {

BindingSet Config(const std::string& text) {
  BindingSet set;
  std::string error;
  EXPECT_TRUE(ParseBindings(text, set, error)) << error;
  return set;
}

GestureEvent Event(GestureKind kind, Phase phase, Handedness hand,
                   Point2 delta = {0.0f, 0.0f},
                   std::uint32_t track_id = 1) {
  GestureEvent e{};
  e.track_id = track_id;
  e.handedness = hand;
  e.kind = kind;
  e.phase = phase;
  e.position = Point2{0.5f, 0.5f};
  e.delta = delta;
  e.timestamp_us = 0;
  return e;
}

// The cursor binding on its own, with smoothing off so arithmetic is exact
// unless a test is specifically about smoothing.
const char* kCursorOnly =
    "set sensitivity 1\n"
    "set smoothing 0\n"
    "right point_index held -> move_cursor\n";

std::vector<Command> MapOne(ActionMapper& m, const GestureEvent& e) {
  std::vector<Command> out;
  m.Map({e}, out);
  return out;
}

}  // namespace

TEST(ActionMapper, AppliesSensitivityToCursorDelta) {
  ActionMapper m(Config(
      "set sensitivity 3\n"
      "set smoothing 0\n"
      "right point_index held -> move_cursor\n"));

  const std::vector<Command> out = MapOne(
      m, Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {0.01f, -0.02f}));

  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, Command::Kind::MoveCursorRelative);
  EXPECT_NEAR(out[0].delta.x, 0.03f, 1e-6f);
  EXPECT_NEAR(out[0].delta.y, -0.06f, 1e-6f);
}

// Landmark jitter is real; one bad frame must not throw the cursor.
TEST(ActionMapper, SmoothingDampensASingleFrameSpike) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0.5\n"
      "right point_index held -> move_cursor\n"));

  const auto feed = [&](float dx) {
    return MapOne(m, Event(GestureKind::PointIndex, Phase::Held,
                           Handedness::Right, {dx, 0.0f}))[0]
        .delta.x;
  };

  // Settle on a steady small motion, then one large spike.
  feed(0.01f);
  feed(0.01f);
  const float settled = feed(0.01f);
  const float spiked = feed(1.0f);

  EXPECT_LT(spiked, 1.0f) << "the spike passed through unfiltered";
  EXPECT_NEAR(spiked, 0.5f * settled + 0.5f * 1.0f, 1e-5f);

  // And the filter decays back rather than latching on the spike.
  const float after = feed(0.01f);
  EXPECT_LT(after, spiked);
}

TEST(ActionMapper, SmoothingOfZeroIsPassThrough) {
  ActionMapper m(Config(kCursorOnly));
  for (float dx : {0.05f, -0.30f, 0.001f, 0.0f}) {
    const std::vector<Command> out = MapOne(
        m, Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {dx, 0.0f}));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].delta.x, dx);
  }
}

// The owner's stated target behaviour: an arrow key held with the left hand
// while the right hand works the cursor.
TEST(ActionMapper, LeftAndRightBindingsAreIndependent) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0\n"
      "right point_index held  -> move_cursor\n"
      "left  thumb_up    began -> key_press   arrow_right\n"
      "left  thumb_up    ended -> key_release arrow_right\n"));

  std::vector<Command> out;
  m.Map({Event(GestureKind::ThumbUp, Phase::Began, Handedness::Left,
               {0.0f, 0.0f}, 7),
         Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {0.04f, 0.0f}, 9)},
        out);

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].kind, Command::Kind::KeyPress);
  EXPECT_EQ(out[0].key, Key::ArrowRight);
  EXPECT_EQ(out[1].kind, Command::Kind::MoveCursorRelative);
  EXPECT_FLOAT_EQ(out[1].delta.x, 0.04f);
}

// Smoothing state is per track, so one hand's motion must not bleed into the
// other's. Without that, the two-hand case would be worse than the one-hand
// case rather than merely independent.
TEST(ActionMapper, SmoothingStateIsPerTrack) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0.5\n"
      "any point_index held -> move_cursor\n"));

  std::vector<Command> out;

  // Track 7 moves hard; track 9 does not move at all.
  for (int i = 0; i < 5; ++i) {
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Left,
                 {0.5f, 0.0f}, 7),
           Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.0f, 0.0f}, 9)},
          out);
    ASSERT_EQ(out.size(), 2u);
  }

  EXPECT_GT(out[0].delta.x, 0.3f) << "the moving hand should be moving";
  EXPECT_NEAR(out[1].delta.x, 0.0f, 1e-6f)
      << "the still hand picked up the other hand's motion";
}

// A Began resets the filter, so a new gesture does not inherit the tail of
// the previous one -- otherwise a quick pinch would fling the cursor the
// moment the user switched to pointing.
TEST(ActionMapper, BeganResetsSmoothingState) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0.5\n"
      "right point_index began -> move_cursor\n"
      "right point_index held  -> move_cursor\n"));

  std::vector<Command> out;
  // Build up a large smoothed value.
  for (int i = 0; i < 6; ++i) {
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.5f, 0.0f})},
          out);
  }
  ASSERT_EQ(out.size(), 1u);
  EXPECT_GT(out[0].delta.x, 0.3f);

  // A new gesture starts. Its own delta is zero, and no history should leak.
  m.Map({Event(GestureKind::PointIndex, Phase::Began, Handedness::Right,
               {0.0f, 0.0f})},
        out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_NEAR(out[0].delta.x, 0.0f, 1e-6f);
}

TEST(ActionMapper, UnboundGestureProducesNoCommand) {
  ActionMapper m(Config(kCursorOnly));
  std::vector<Command> out;

  // Right gesture, wrong phase.
  m.Map({Event(GestureKind::PointIndex, Phase::Began, Handedness::Right)},
        out);
  EXPECT_TRUE(out.empty());

  // Right phase, wrong gesture.
  m.Map({Event(GestureKind::Fist, Phase::Held, Handedness::Right)}, out);
  EXPECT_TRUE(out.empty());

  // Right gesture and phase, wrong hand.
  m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Left)}, out);
  EXPECT_TRUE(out.empty());

  // And an unlabelled hand against a `right` binding.
  m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Unknown)},
        out);
  EXPECT_TRUE(out.empty());
}

// Phase 1 measured handedness confidence as low as 0.575, so this is the
// configuration a user with unreliable labelling has to be able to write.
TEST(ActionMapper, AnyBindingMatchesUnknownHandedness) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0\n"
      "any point_index held -> move_cursor\n"));

  for (Handedness h : {Handedness::Left, Handedness::Right,
                       Handedness::Unknown}) {
    const std::vector<Command> out = MapOne(
        m, Event(GestureKind::PointIndex, Phase::Held, h, {0.02f, 0.0f}));
    ASSERT_EQ(out.size(), 1u) << "handedness " << static_cast<int>(h);
    EXPECT_FLOAT_EQ(out[0].delta.x, 0.02f);
  }
}

// Review focus 1 and 7: the release must be generated, or the key sticks
// down after the hand has gone.
TEST(ActionMapper, EndedPhaseEmitsKeyReleaseForAHeldKeyBinding) {
  ActionMapper m(Config(
      "left thumb_up began -> key_press   arrow_right\n"
      "left thumb_up ended -> key_release arrow_right\n"));

  const std::vector<Command> press = MapOne(
      m, Event(GestureKind::ThumbUp, Phase::Began, Handedness::Left));
  ASSERT_EQ(press.size(), 1u);
  EXPECT_EQ(press[0].kind, Command::Kind::KeyPress);
  EXPECT_EQ(press[0].key, Key::ArrowRight);

  const std::vector<Command> release = MapOne(
      m, Event(GestureKind::ThumbUp, Phase::Ended, Handedness::Left));
  ASSERT_EQ(release.size(), 1u);
  EXPECT_EQ(release[0].kind, Command::Kind::KeyRelease);
  EXPECT_EQ(release[0].key, Key::ArrowRight);
}

// The whole shipped configuration, driven through the mapper, so the default
// a user actually gets is exercised rather than only hand-written snippets.
TEST(ActionMapper, DefaultConfigurationProducesTheExpectedCommands) {
  ActionMapper m(Config(hc::DefaultBindingsText()));

  const std::vector<Command> click = MapOne(
      m, Event(GestureKind::TwoFingers, Phase::Began, Handedness::Right));
  ASSERT_EQ(click.size(), 1u);
  EXPECT_EQ(click[0].kind, Command::Kind::Click);
  EXPECT_EQ(click[0].button, MouseButton::Left);

  const std::vector<Command> dbl = MapOne(
      m, Event(GestureKind::Pinch, Phase::Began, Handedness::Right));
  ASSERT_EQ(dbl.size(), 1u);
  EXPECT_EQ(dbl[0].kind, Command::Kind::DoubleClick);

  const std::vector<Command> scroll = MapOne(
      m, Event(GestureKind::OpenPalm, Phase::Held, Handedness::Right,
               {0.0f, 0.05f}));
  ASSERT_EQ(scroll.size(), 1u);
  EXPECT_EQ(scroll[0].kind, Command::Kind::Scroll);

  const std::vector<Command> down = MapOne(
      m, Event(GestureKind::FourFingers, Phase::Began, Handedness::Left));
  ASSERT_EQ(down.size(), 1u);
  EXPECT_EQ(down[0].kind, Command::Kind::KeyPress);
  EXPECT_EQ(down[0].key, Key::ArrowDown);
}

// Click and key commands carry no motion. A click whose delta leaked the
// cursor's last movement would make a sink that reads delta unconditionally
// move the pointer mid-click.
TEST(ActionMapper, NonMotionCommandsCarryZeroDelta) {
  ActionMapper m(Config(
      "right point_index held  -> move_cursor\n"
      "right two_fingers began -> click left\n"));

  std::vector<Command> out;
  m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {0.4f, 0.4f})},
        out);
  ASSERT_EQ(out.size(), 1u);

  m.Map({Event(GestureKind::TwoFingers, Phase::Began, Handedness::Right,
               {0.4f, 0.4f})},
        out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind, Command::Kind::Click);
  EXPECT_FLOAT_EQ(out[0].delta.x, 0.0f);
  EXPECT_FLOAT_EQ(out[0].delta.y, 0.0f);
}

// Deltas stay normalized. The mapper has no screen dimensions and must not
// acquire any -- that conversion belongs to the sink.
TEST(ActionMapper, EmitsNoPixelsAnywhere) {
  // Sensitivity is a multiplier on normalized motion, so even an aggressive
  // setting against a full-frame sweep stays in single digits. If the mapper
  // had silently acquired a screen size, this would come out in the hundreds
  // or thousands.
  ActionMapper m(Config(
      "set sensitivity 10\n"
      "set smoothing 0\n"
      "right point_index held -> move_cursor\n"));

  const std::vector<Command> out = MapOne(
      m, Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {1.0f, 1.0f}));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FLOAT_EQ(out[0].delta.x, 10.0f);
  EXPECT_LT(std::abs(out[0].delta.x), 100.0f)
      << "this looks like a pixel count, not normalized motion";

  // And structurally: the only thing the mapper is constructed from is a
  // BindingSet. There is nowhere for a resolution to enter.
  static_assert(std::is_constructible<ActionMapper, BindingSet>::value, "");
}

// --- buffer and configuration contracts ----------------------------------

TEST(ActionMapper, MapClearsTheOutputBuffer) {
  ActionMapper m(Config(kCursorOnly));
  std::vector<Command> out(5);
  m.Map({}, out);
  EXPECT_TRUE(out.empty());
}

TEST(ActionMapper, EmptyEventListProducesNothing) {
  ActionMapper m(Config(hc::DefaultBindingsText()));
  std::vector<Command> out;
  m.Map({}, out);
  EXPECT_TRUE(out.empty());
}

TEST(ActionMapper, SetBindingsTakesEffectAndClearsSmoothing) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0.9\n"
      "right point_index held -> move_cursor\n"));

  std::vector<Command> out;
  for (int i = 0; i < 10; ++i) {
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.5f, 0.0f})},
          out);
  }
  ASSERT_EQ(out.size(), 1u);
  EXPECT_GT(out[0].delta.x, 0.1f);

  // New configuration: different sensitivity, smoothing off.
  m.SetBindings(Config(
      "set sensitivity 2\n"
      "set smoothing 0\n"
      "right point_index held -> move_cursor\n"));
  EXPECT_FLOAT_EQ(m.Bindings().sensitivity, 2.0f);

  m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {0.01f, 0.0f})},
        out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FLOAT_EQ(out[0].delta.x, 0.02f)
      << "smoothing state survived the configuration change";
}

TEST(ActionMapper, ResetClearsSmoothingButKeepsBindings) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0.9\n"
      "right point_index held -> move_cursor\n"));

  std::vector<Command> out;
  for (int i = 0; i < 10; ++i) {
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.5f, 0.0f})},
          out);
  }
  ASSERT_GT(out[0].delta.x, 0.1f);

  m.Reset();
  EXPECT_FLOAT_EQ(m.Bindings().smoothing, 0.9f) << "bindings must survive";

  m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
               {0.0f, 0.0f})},
        out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_NEAR(out[0].delta.x, 0.0f, 1e-6f);
}

// An empty configuration is valid, and a mapper holding one is inert rather
// than broken -- the state a bindings UI starts a user from.
TEST(ActionMapper, EmptyBindingSetProducesNothingForAnything) {
  ActionMapper m(BindingSet{});
  std::vector<Command> out;
  m.Map({Event(GestureKind::Pinch, Phase::Began, Handedness::Right),
         Event(GestureKind::OpenPalm, Phase::Held, Handedness::Left),
         Event(GestureKind::Fist, Phase::Ended, Handedness::Unknown)},
        out);
  EXPECT_TRUE(out.empty());
}

// More tracks than the fixed smoothing table holds. The surplus must still
// produce commands -- dropping a click because 33 hands were somehow in frame
// would be a worse failure than sharing a filter.
TEST(ActionMapper, HandlesMoreTracksThanTheSmoothingTable) {
  ActionMapper m(Config(
      "set sensitivity 1\n"
      "set smoothing 0\n"
      "any two_fingers began -> click left\n"));

  std::vector<GestureEvent> events;
  for (std::size_t i = 0; i < hc::kMaxGestureTracks + 4; ++i) {
    events.push_back(Event(GestureKind::TwoFingers, Phase::Began,
                           Handedness::Right, {0.0f, 0.0f},
                           static_cast<std::uint32_t>(i + 1)));
  }

  std::vector<Command> out;
  m.Map(events, out);
  EXPECT_EQ(out.size(), events.size());
}

// --- smoothing state must survive track-id churn ---------------------------
//
// Every hand dropout re-issues a track id, and the smoothing table never frees
// a slot. These reproduce the cursor going unfiltered once 32 ids have been
// seen in a session.

namespace {

const char* kSmoothedCursor =
    "set sensitivity 2\n"
    "set smoothing 0.5\n"
    "right point_index held -> move_cursor\n";

// Feeds Began plus alternating-noise Held events on one track and returns the
// cursor x deltas the mapper produced.
std::vector<float> NoisyCursorRun(ActionMapper& m, std::uint32_t track_id,
                                  int held_frames) {
  std::vector<Command> ignored;
  m.Map({Event(GestureKind::PointIndex, Phase::Began, Handedness::Right,
               {0.0f, 0.0f}, track_id)},
        ignored);

  std::vector<float> xs;
  for (int i = 0; i < held_frames; ++i) {
    const float dx = (i % 2 == 0) ? 0.01f : -0.01f;
    const std::vector<Command> out =
        MapOne(m, Event(GestureKind::PointIndex, Phase::Held,
                        Handedness::Right, {dx, 0.0f}, track_id));
    if (out.size() == 1 && out[0].kind == Command::Kind::MoveCursorRelative) {
      xs.push_back(out[0].delta.x);
    }
  }
  return xs;
}

void ExpectEmaAndBoundedJump(const std::vector<float>& xs, int held_frames) {
  ASSERT_EQ(xs.size(), static_cast<std::size_t>(held_frames));
  float smoothed = 0.0f;
  float max_jump = 0.0f;
  for (int i = 0; i < held_frames; ++i) {
    const float raw = (i % 2 == 0) ? 0.01f : -0.01f;
    smoothed = 0.5f * smoothed + 0.5f * raw;
    EXPECT_NEAR(xs[i], smoothed * 2.0f, 1e-6f) << "frame " << i;
    if (i > 0) max_jump = std::fmax(max_jump, std::fabs(xs[i] - xs[i - 1]));
  }
  // Unfiltered, the alternating input jumps 0.04 per frame. The EMA settles
  // to roughly 0.0133 swings, so anything near 0.04 means no filtering.
  EXPECT_LT(max_jump, 0.025f) << "frame-to-frame jump is unfiltered-sized";
}

}  // namespace

TEST(ActionMapper, FreshTrackIsSmoothed) {
  ActionMapper m(Config(kSmoothedCursor));
  ExpectEmaAndBoundedJump(NoisyCursorRun(m, 1, 8), 8);
}

TEST(ActionMapper, SmoothingSurvivesManyTrackIds) {
  ActionMapper m(Config(kSmoothedCursor));

  // 32 short-lived tracks, as 32 hand dropouts would produce.
  for (std::uint32_t id = 1; id <= hc::kMaxGestureTracks; ++id) {
    std::vector<Command> out;
    m.Map({Event(GestureKind::PointIndex, Phase::Began, Handedness::Right,
                 {0.0f, 0.0f}, id)},
          out);
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.01f, 0.0f}, id)},
          out);
    m.Map({Event(GestureKind::PointIndex, Phase::Ended, Handedness::Right,
                 {0.0f, 0.0f}, id)},
          out);
  }

  const std::uint32_t next =
      static_cast<std::uint32_t>(hc::kMaxGestureTracks) + 1;
  ExpectEmaAndBoundedJump(NoisyCursorRun(m, next, 8), 8);
}

TEST(ActionMapper, EndedFreesTheSmoothingSlot) {
  ActionMapper m(Config(kSmoothedCursor));
  std::vector<Command> out;
  const auto held = [&] {
    return MapOne(m, Event(GestureKind::PointIndex, Phase::Held,
                           Handedness::Right, {0.01f, 0.0f}, 1));
  };

  // Converge the filter towards 0.01 (output towards 0.02).
  std::vector<Command> last;
  for (int i = 0; i < 8; ++i) last = held();
  ASSERT_EQ(last.size(), 1u);
  EXPECT_GT(last[0].delta.x, 0.019f);

  m.Map({Event(GestureKind::PointIndex, Phase::Ended, Handedness::Right,
               {0.0f, 0.0f}, 1)},
        out);

  // Same id, no Began in between: only a freed slot starts from rest again.
  // First filtered sample is 0.5 * 0.01, times sensitivity 2.
  const std::vector<Command> after = held();
  ASSERT_EQ(after.size(), 1u);
  EXPECT_NEAR(after[0].delta.x, 0.01f, 1e-6f);
}

TEST(ActionMapper, FullTableEvictsASlotNotTouchedInThisCall) {
  ActionMapper m(Config(kSmoothedCursor));
  std::vector<Command> out;

  // Fill every slot with a track that never ends.
  for (std::uint32_t id = 1; id <= hc::kMaxGestureTracks; ++id) {
    m.Map({Event(GestureKind::PointIndex, Phase::Held, Handedness::Right,
                 {0.01f, 0.0f}, id)},
          out);
  }

  // A new id, in a call that touches none of the old ones, must be filtered.
  ExpectEmaAndBoundedJump(NoisyCursorRun(m, 1000, 8), 8);
}

TEST(ActionMapper, FullTableNeverEvictsASlotTouchedInThisCall) {
  ActionMapper m(Config(kSmoothedCursor));
  std::vector<Command> out;

  // One call touching every slot plus one more: the extra has nowhere to go
  // and degrades to unfiltered (raw * sensitivity), while the rest stay
  // smoothed.
  std::vector<GestureEvent> events;
  for (std::uint32_t id = 1; id <= hc::kMaxGestureTracks + 1; ++id) {
    events.push_back(Event(GestureKind::PointIndex, Phase::Held,
                           Handedness::Right, {0.01f, 0.0f}, id));
  }
  m.Map(events, out);
  ASSERT_EQ(out.size(), events.size());
  for (std::size_t i = 0; i < hc::kMaxGestureTracks; ++i) {
    EXPECT_NEAR(out[i].delta.x, 0.01f, 1e-6f);
  }
  EXPECT_NEAR(out.back().delta.x, 0.02f, 1e-6f);
}
