#include "hc/logging_sink.hpp"
#include "hc/null_sink.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hc/command.hpp"

using hc::Command;
using hc::Key;
using hc::LoggingSink;
using hc::MouseButton;
using hc::NullSink;
using hc::Point2;

namespace {

Command Move(float dx, float dy) {
  Command c{};
  c.kind = Command::Kind::MoveCursorRelative;
  c.delta = Point2{dx, dy};
  return c;
}

Command Click(MouseButton b = MouseButton::Left) {
  Command c{};
  c.kind = Command::Kind::Click;
  c.button = b;
  return c;
}

Command Press(Key k) {
  Command c{};
  c.kind = Command::Kind::KeyPress;
  c.key = k;
  return c;
}

Command Release(Key k) {
  Command c{};
  c.kind = Command::Kind::KeyRelease;
  c.key = k;
  return c;
}

}  // namespace

// --- NullSink -------------------------------------------------------------

TEST(NullSink, SwallowsEverythingAndIsAlwaysAvailable) {
  NullSink sink;

  // Available is TRUE: discarding is this sink's job and it does it
  // perfectly. False would mean broken, which is a different thing the
  // application reports differently.
  EXPECT_TRUE(sink.Available());

  sink.Send(Move(0.5f, -0.5f));
  sink.Send(Click());
  sink.Send(Press(Key::ArrowUp));
  sink.ReleaseAll();
  sink.ReleaseAll();
  EXPECT_TRUE(sink.Available());
}

// --- LoggingSink ----------------------------------------------------------

TEST(LoggingSink, RecordsCommandsInOrder) {
  LoggingSink sink;
  sink.Send(Move(0.1f, 0.2f));
  sink.Send(Click(MouseButton::Middle));
  sink.Send(Press(Key::ArrowLeft));

  ASSERT_EQ(sink.Commands().size(), 3u);
  EXPECT_EQ(sink.Commands()[0].kind, Command::Kind::MoveCursorRelative);
  EXPECT_FLOAT_EQ(sink.Commands()[0].delta.x, 0.1f);
  EXPECT_EQ(sink.Commands()[1].kind, Command::Kind::Click);
  EXPECT_EQ(sink.Commands()[1].button, MouseButton::Middle);
  EXPECT_EQ(sink.Commands()[2].kind, Command::Kind::KeyPress);
  EXPECT_EQ(sink.Commands()[2].key, Key::ArrowLeft);
  EXPECT_TRUE(sink.Available());
}

TEST(LoggingSink, ReleaseAllEmitsAReleaseForEachHeldPressAndNothingForNone) {
  LoggingSink sink;
  sink.Send(Press(Key::ArrowUp));
  sink.Send(Press(Key::ArrowLeft));
  sink.Send(Click());  // Atomic; holds nothing.
  ASSERT_EQ(sink.HeldKeys().size(), 2u);

  sink.ReleaseAll();

  // Two releases appended, in press order, and nothing for the click.
  ASSERT_EQ(sink.Commands().size(), 5u);
  EXPECT_EQ(sink.Commands()[3].kind, Command::Kind::KeyRelease);
  EXPECT_EQ(sink.Commands()[3].key, Key::ArrowUp);
  EXPECT_EQ(sink.Commands()[4].kind, Command::Kind::KeyRelease);
  EXPECT_EQ(sink.Commands()[4].key, Key::ArrowLeft);
  EXPECT_TRUE(sink.HeldKeys().empty());

  // And nothing at all when nothing is held.
  LoggingSink empty;
  empty.ReleaseAll();
  EXPECT_TRUE(empty.Commands().empty());
}

// ReleaseAll runs on shutdown paths including the abnormal ones, where being
// called more than once is normal rather than exceptional.
TEST(LoggingSink, DoubleReleaseAllIsIdempotent) {
  LoggingSink sink;
  sink.Send(Press(Key::ArrowDown));
  sink.ReleaseAll();
  const std::size_t after_first = sink.Commands().size();

  sink.ReleaseAll();
  EXPECT_EQ(sink.Commands().size(), after_first)
      << "the second ReleaseAll invented a release";
}

// A binding on the `held` phase presses the same key every frame. Recording
// each press separately would make ReleaseAll emit one release per frame.
TEST(LoggingSink, RepeatedPressOfTheSameKeyIsHeldOnce) {
  LoggingSink sink;
  for (int i = 0; i < 20; ++i) sink.Send(Press(Key::ArrowRight));
  EXPECT_EQ(sink.HeldKeys().size(), 1u);

  sink.ReleaseAll();
  int releases = 0;
  for (const Command& c : sink.Commands()) {
    if (c.kind == Command::Kind::KeyRelease) ++releases;
  }
  EXPECT_EQ(releases, 1);
}

TEST(LoggingSink, AnExplicitReleaseClearsTheHeldKey) {
  LoggingSink sink;
  sink.Send(Press(Key::ArrowUp));
  sink.Send(Release(Key::ArrowUp));
  EXPECT_TRUE(sink.HeldKeys().empty());

  sink.ReleaseAll();  // Must add nothing: it is already up.
  EXPECT_EQ(sink.Commands().size(), 2u);
}

// Releasing a key that was never pressed is what a configuration with a lone
// `key_release` binding produces, and ParseBindings deliberately allows it.
TEST(LoggingSink, ReleasingAnUnheldKeyIsHarmless) {
  LoggingSink sink;
  sink.Send(Release(Key::Space));
  EXPECT_TRUE(sink.HeldKeys().empty());
  EXPECT_EQ(sink.Commands().size(), 1u);
}

TEST(LoggingSink, WritesEachCommandToTheStreamWhenGivenOne) {
  std::ostringstream out;
  LoggingSink sink(&out);
  sink.Send(Click(MouseButton::Right));
  sink.Send(Press(Key::PrintScreen));

  const std::string text = out.str();
  EXPECT_NE(text.find("click right"), std::string::npos) << text;
  EXPECT_NE(text.find("key_press print_screen"), std::string::npos) << text;
}

TEST(LoggingSink, DescribeNamesTheCommandAndItsArgument) {
  EXPECT_EQ(LoggingSink::Describe(Click(MouseButton::Middle)), "click middle");
  EXPECT_EQ(LoggingSink::Describe(Press(Key::Tab)), "key_press tab");
  EXPECT_EQ(LoggingSink::Describe(Release(Key::Escape)), "key_release escape");
  EXPECT_EQ(LoggingSink::Describe(Move(0.25f, -0.5f)),
            "move_cursor 0.2500 -0.5000");
}

TEST(LoggingSink, ClearDropsRecordedCommandsAndHeldKeys) {
  LoggingSink sink;
  sink.Send(Press(Key::ArrowUp));
  sink.Clear();
  EXPECT_TRUE(sink.Commands().empty());
  EXPECT_TRUE(sink.HeldKeys().empty());
}
