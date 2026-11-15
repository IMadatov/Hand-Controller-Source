#include "hc/bindings.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/hand_observation.hpp"

using hc::Binding;
using hc::BindingSet;
using hc::Command;
using hc::DefaultBindingsText;
using hc::GestureKind;
using hc::Handedness;
using hc::HandSelector;
using hc::HandSelectorMatches;
using hc::Key;
using hc::MouseButton;
using hc::ParseBindings;
using hc::SerializeBindings;
using Phase = hc::GestureEvent::Phase;

namespace {

// Parses and fails the test with the parser's own message if it did not.
BindingSet MustParse(const std::string& text) {
  BindingSet set;
  std::string error;
  const bool ok = ParseBindings(text, set, error);
  EXPECT_TRUE(ok) << "unexpected parse failure: " << error;
  return set;
}

// Returns the parser's error message, having asserted that parsing failed.
std::string MustFail(const std::string& text) {
  BindingSet set;
  std::string error;
  const bool ok = ParseBindings(text, set, error);
  EXPECT_FALSE(ok) << "expected a parse failure, got none";
  return error;
}

bool SameCommand(const Command& a, const Command& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case Command::Kind::Click:
    case Command::Kind::DoubleClick:
      return a.button == b.button;
    case Command::Kind::KeyPress:
    case Command::Kind::KeyRelease:
      return a.key == b.key;
    case Command::Kind::MoveCursorRelative:
    case Command::Kind::Scroll:
    case Command::Kind::None:
      return true;
  }
  return false;
}

// A minimal well-formed configuration, for tests that need a valid baseline
// to perturb.
constexpr const char* kMinimal = "right point_index held -> move_cursor\n";

}  // namespace

// --- the shipped default --------------------------------------------------

TEST(Bindings, ParsesTheDefaultFileShipped) {
  BindingSet set;
  std::string error;
  ASSERT_TRUE(ParseBindings(DefaultBindingsText(), set, error)) << error;
  EXPECT_FALSE(set.bindings.empty());
}

// The compiled-in text is generated from the installed file by CMake. If that
// wiring ever breaks, this is what notices.
TEST(Bindings, CompiledInDefaultMatchesTheInstalledFile) {
  std::ifstream in(HC_DEFAULT_BINDINGS_PATH);
  ASSERT_TRUE(in.good()) << "cannot open " << HC_DEFAULT_BINDINGS_PATH;
  std::ostringstream buf;
  buf << in.rdbuf();
  EXPECT_EQ(buf.str(), std::string(DefaultBindingsText()));
}

// Parity with main.py is the stated goal of phase 2, so the default must
// actually reach every command the Python version could issue.
TEST(Bindings, DefaultCoversEveryCommandMainPyCould) {
  const BindingSet set = MustParse(DefaultBindingsText());

  bool move = false, scroll = false, click = false, dbl = false;
  bool up = false, down = false, left = false, right = false, prtsc = false;

  for (const Binding& b : set.bindings) {
    switch (b.command.kind) {
      case Command::Kind::MoveCursorRelative: move = true; break;
      case Command::Kind::Scroll: scroll = true; break;
      case Command::Kind::Click: click = true; break;
      case Command::Kind::DoubleClick: dbl = true; break;
      case Command::Kind::KeyPress:
        if (b.command.key == Key::ArrowUp) up = true;
        if (b.command.key == Key::ArrowDown) down = true;
        if (b.command.key == Key::ArrowLeft) left = true;
        if (b.command.key == Key::ArrowRight) right = true;
        if (b.command.key == Key::PrintScreen) prtsc = true;
        break;
      default: break;
    }
  }

  EXPECT_TRUE(move) << "cursor movement";
  EXPECT_TRUE(scroll) << "scrolling";
  EXPECT_TRUE(click) << "left click";
  EXPECT_TRUE(dbl) << "double click";
  EXPECT_TRUE(up && down && left && right) << "all four arrow keys";
  EXPECT_TRUE(prtsc) << "print screen";
}

// --- the plan's error cases -----------------------------------------------

TEST(Bindings, RejectsUnknownGestureNamingIt) {
  const std::string error = MustFail("right pnch began -> click left\n");
  EXPECT_NE(error.find("pnch"), std::string::npos)
      << "the message must name the offending token: " << error;
}

TEST(Bindings, RejectsUnknownCommandNamingIt) {
  const std::string error = MustFail("right pinch began -> clik left\n");
  EXPECT_NE(error.find("clik"), std::string::npos) << error;
}

TEST(Bindings, RejectsUnknownHandNamingIt) {
  const std::string error = MustFail("middle pinch began -> click left\n");
  EXPECT_NE(error.find("middle"), std::string::npos) << error;
}

TEST(Bindings, RejectsUnknownPhaseNamingIt) {
  const std::string error = MustFail("right pinch starting -> click left\n");
  EXPECT_NE(error.find("starting"), std::string::npos) << error;
}

TEST(Bindings, RejectsUnknownKeyNamingIt) {
  const std::string error =
      MustFail("left thumb_up began -> key_press super\n");
  EXPECT_NE(error.find("super"), std::string::npos) << error;
}

// "none" is a real enum value in both GestureKind and Command::Kind, so the
// parser has to reject it explicitly rather than relying on the name lookup
// to fail.
TEST(Bindings, RejectsNoneAsAGestureOrACommand) {
  EXPECT_NE(MustFail("right none began -> click left\n").find("none"),
            std::string::npos);
  EXPECT_NE(MustFail("right pinch began -> none\n").find("none"),
            std::string::npos);
}

// A configuration error the user cannot locate is a configuration format that
// failed.
TEST(Bindings, ReportsTheLineNumberOnError) {
  const std::string text =
      "# a comment\n"
      "\n"
      "right point_index held -> move_cursor\n"
      "right pnch began -> click left\n";
  const std::string error = MustFail(text);
  EXPECT_NE(error.find("line 4"), std::string::npos)
      << "the error is on line 4: " << error;
}

TEST(Bindings, IgnoresCommentsAndBlankLines) {
  const std::string text =
      "# leading comment\n"
      "\n"
      "   \n"
      "right point_index held -> move_cursor   # trailing comment\n"
      "\t\n"
      "# trailing comment line\n";
  const BindingSet set = MustParse(text);
  ASSERT_EQ(set.bindings.size(), 1u);
  EXPECT_EQ(set.bindings[0].command.kind, Command::Kind::MoveCursorRelative);
}

// Silently letting the last line win makes a typo invisible.
TEST(Bindings, RejectsTwoBindingsForTheSameHandGesturePhase) {
  const std::string error = MustFail(
      "right pinch began -> click left\n"
      "right pinch began -> double_click left\n");
  EXPECT_NE(error.find("line 1"), std::string::npos)
      << "the message must point at the binding it collides with: " << error;
}

// The subtle half of the same rule: `any` is not textually equal to `left`,
// but a left hand satisfies both, so one gesture would fire two commands.
TEST(Bindings, RejectsAnyOverlappingASpecificHand) {
  EXPECT_NE(MustFail("any pinch began -> click left\n"
                     "left pinch began -> double_click left\n")
                .find("overlap"),
            std::string::npos);
  // And in the other order, so the check is not direction-dependent.
  EXPECT_NE(MustFail("left pinch began -> click left\n"
                     "any pinch began -> double_click left\n")
                .find("overlap"),
            std::string::npos);
}

// Different hands, or different phases, are not overlaps at all.
TEST(Bindings, AllowsTheSameGestureOnDifferentHandsAndPhases) {
  const BindingSet set = MustParse(
      "right pinch began -> click left\n"
      "left  pinch began -> click right\n"
      "right pinch held  -> move_cursor\n");
  EXPECT_EQ(set.bindings.size(), 3u);
}

// --- settings --------------------------------------------------------------

TEST(Bindings, SensitivityAndSmoothingHaveDefaultsAndAreOverridable) {
  const BindingSet defaulted = MustParse(kMinimal);
  EXPECT_FLOAT_EQ(defaulted.sensitivity, hc::kDefaultSensitivity);
  EXPECT_FLOAT_EQ(defaulted.smoothing, hc::kDefaultSmoothing);

  const BindingSet overridden = MustParse(
      "set sensitivity 3.5\n"
      "set smoothing 0.25\n" +
      std::string(kMinimal));
  EXPECT_FLOAT_EQ(overridden.sensitivity, 3.5f);
  EXPECT_FLOAT_EQ(overridden.smoothing, 0.25f);
}

TEST(Bindings, RejectsOutOfRangeSmoothing) {
  EXPECT_FALSE(MustFail("set smoothing 1.5\n").empty());
  EXPECT_FALSE(MustFail("set smoothing -0.1\n").empty());
  // The ends are inclusive and must be accepted.
  EXPECT_FLOAT_EQ(MustParse("set smoothing 0\n").smoothing, 0.0f);
  EXPECT_FLOAT_EQ(MustParse("set smoothing 1\n").smoothing, 1.0f);
}

TEST(Bindings, RejectsNonPositiveSensitivity) {
  EXPECT_FALSE(MustFail("set sensitivity 0\n").empty());
  EXPECT_FALSE(MustFail("set sensitivity -2\n").empty());
}

TEST(Bindings, RejectsNonNumericAndMalformedSettings) {
  EXPECT_NE(MustFail("set smoothing half\n").find("half"), std::string::npos);
  EXPECT_FALSE(MustFail("set smoothing 0.5x\n").empty());
  EXPECT_FALSE(MustFail("set smoothing\n").empty());
  EXPECT_FALSE(MustFail("set smoothing 0.5 0.6\n").empty());
  EXPECT_NE(MustFail("set sensitivty 2.0\n").find("sensitivty"),
            std::string::npos);
}

// Every comparison against NaN is false, so a naive `v >= 0 && v <= 1` range
// check lets "nan" straight through and the mapper then multiplies by it.
TEST(Bindings, RejectsNonFiniteSettings) {
  EXPECT_FALSE(MustFail("set smoothing nan\n").empty());
  EXPECT_FALSE(MustFail("set smoothing inf\n").empty());
  EXPECT_FALSE(MustFail("set sensitivity inf\n").empty());
}

// --- command arguments ----------------------------------------------------

TEST(Bindings, ClickButtonDefaultsToLeftAndIsOverridable) {
  const BindingSet set = MustParse(
      "right pinch began  -> click\n"
      "right fist  began  -> click middle\n");
  ASSERT_EQ(set.bindings.size(), 2u);
  EXPECT_EQ(set.bindings[0].command.button, MouseButton::Left);
  EXPECT_EQ(set.bindings[1].command.button, MouseButton::Middle);
}

TEST(Bindings, KeyCommandsRequireAKey) {
  EXPECT_FALSE(MustFail("left thumb_up began -> key_press\n").empty());
  EXPECT_FALSE(MustFail("left thumb_up ended -> key_release\n").empty());
}

TEST(Bindings, MovementCommandsRejectAnArgument) {
  EXPECT_FALSE(MustFail("right point_index held -> move_cursor left\n").empty());
  EXPECT_FALSE(MustFail("right open_palm held -> scroll fast\n").empty());
}

TEST(Bindings, RejectsTrailingGarbageOnABinding) {
  EXPECT_FALSE(
      MustFail("right pinch began -> click left extra\n").empty());
}

TEST(Bindings, RejectsAMissingArrow) {
  EXPECT_FALSE(MustFail("right pinch began click left\n").empty());
}

// --- the press/release pairing rule ---------------------------------------

// A key still down after the gesture stops outlives the program. The plan
// calls a stuck modifier the most likely way this build hurts someone, so a
// configuration that would cause one is rejected at load.
TEST(Bindings, RejectsAKeyPressWithNoMatchingRelease) {
  const std::string error =
      MustFail("left thumb_up began -> key_press arrow_right\n");
  EXPECT_NE(error.find("key_release"), std::string::npos)
      << "the message should say what to add: " << error;
  EXPECT_NE(error.find("line 1"), std::string::npos) << error;
}

TEST(Bindings, AcceptsAPairedPressAndRelease) {
  const BindingSet set = MustParse(
      "left thumb_up began -> key_press   arrow_right\n"
      "left thumb_up ended -> key_release arrow_right\n");
  EXPECT_EQ(set.bindings.size(), 2u);
}

// The release has to be for the same key, not merely present.
TEST(Bindings, RejectsAReleaseOfADifferentKey) {
  EXPECT_FALSE(MustFail(
                   "left thumb_up began -> key_press   arrow_right\n"
                   "left thumb_up ended -> key_release arrow_left\n")
                   .empty());
}

// And for the same hand and gesture: releasing on a different gesture leaves
// the key down whenever that other gesture never happens.
TEST(Bindings, RejectsAReleaseOnADifferentGesture) {
  EXPECT_FALSE(MustFail(
                   "left thumb_up  began -> key_press   arrow_right\n"
                   "left pinky_only ended -> key_release arrow_right\n")
                   .empty());
}

TEST(Bindings, RejectsAReleaseOnAPhaseOtherThanEnded) {
  EXPECT_FALSE(MustFail(
                   "left thumb_up began -> key_press   arrow_right\n"
                   "left thumb_up held  -> key_release arrow_right\n")
                   .empty());
}

// A lone release is harmless -- releasing an unpressed key does nothing -- so
// it must not be an error.
TEST(Bindings, AllowsAReleaseWithoutAPress) {
  const BindingSet set =
      MustParse("left thumb_up ended -> key_release arrow_right\n");
  EXPECT_EQ(set.bindings.size(), 1u);
}

// --- hand matching --------------------------------------------------------

// Phase 1 measured handedness confidence as low as 0.575, so Unknown is a
// live value. Without `any`, an unlabelled hand would match nothing and every
// gesture it made would silently do nothing.
TEST(Bindings, AnyMatchesUnknownHandednessButLeftAndRightDoNot) {
  EXPECT_TRUE(HandSelectorMatches(HandSelector::Any, Handedness::Unknown));
  EXPECT_TRUE(HandSelectorMatches(HandSelector::Any, Handedness::Left));
  EXPECT_TRUE(HandSelectorMatches(HandSelector::Any, Handedness::Right));

  EXPECT_FALSE(HandSelectorMatches(HandSelector::Left, Handedness::Unknown));
  EXPECT_FALSE(HandSelectorMatches(HandSelector::Right, Handedness::Unknown));

  EXPECT_TRUE(HandSelectorMatches(HandSelector::Left, Handedness::Left));
  EXPECT_FALSE(HandSelectorMatches(HandSelector::Left, Handedness::Right));
  EXPECT_TRUE(HandSelectorMatches(HandSelector::Right, Handedness::Right));
  EXPECT_FALSE(HandSelectorMatches(HandSelector::Right, Handedness::Left));
}

// --- the round trip -------------------------------------------------------

// Without this property the eventual bindings UI cannot save what it loaded,
// and would reimplement the format to do it.
TEST(Bindings, RoundTripsThroughSerialize) {
  const BindingSet original = MustParse(DefaultBindingsText());
  const std::string text = SerializeBindings(original);

  BindingSet reparsed;
  std::string error;
  ASSERT_TRUE(ParseBindings(text, reparsed, error))
      << "serialized output did not parse: " << error << "\n" << text;

  EXPECT_FLOAT_EQ(reparsed.sensitivity, original.sensitivity);
  EXPECT_FLOAT_EQ(reparsed.smoothing, original.smoothing);
  ASSERT_EQ(reparsed.bindings.size(), original.bindings.size());
  for (std::size_t i = 0; i < original.bindings.size(); ++i) {
    const Binding& a = original.bindings[i];
    const Binding& b = reparsed.bindings[i];
    EXPECT_EQ(a.hand, b.hand) << "binding " << i;
    EXPECT_EQ(a.gesture, b.gesture) << "binding " << i;
    EXPECT_EQ(a.phase, b.phase) << "binding " << i;
    EXPECT_TRUE(SameCommand(a.command, b.command)) << "binding " << i;
  }
}

// Exercises every gesture, phase, command and key rather than only the ones
// the default happens to use -- a round trip that only covers the default is
// a round trip for one configuration.
TEST(Bindings, RoundTripsEveryGestureCommandAndKey) {
  std::ostringstream text;
  text << "set sensitivity 1.75\nset smoothing 0.125\n";

  const char* gestures[] = {"pinch", "fist", "open_palm", "point_index",
                            "two_fingers", "thumb_up", "pinky_only",
                            "four_fingers"};
  const char* keys[] = {"arrow_up", "arrow_down", "arrow_left", "arrow_right",
                        "print_screen", "escape", "enter", "space", "tab"};
  const char* buttons[] = {"left", "right", "middle"};

  // One gesture per line, cycling the commands so each kind appears.
  std::size_t k = 0;
  for (std::size_t g = 0; g < 8; ++g) {
    switch (g % 4) {
      case 0:
        text << "any " << gestures[g] << " held -> move_cursor\n";
        break;
      case 1:
        text << "any " << gestures[g] << " held -> scroll\n";
        break;
      case 2:
        text << "any " << gestures[g] << " began -> click "
             << buttons[g % 3] << "\n";
        break;
      case 3:
        text << "any " << gestures[g] << " began -> double_click "
             << buttons[g % 3] << "\n";
        break;
    }
    // And a paired press/release on a phase that cannot collide with the
    // above: presses go on `held`, releases on `ended`.
    if (g % 4 != 0 && g % 4 != 1) {
      text << "any " << gestures[g] << " held -> key_press " << keys[k]
           << "\n";
      text << "any " << gestures[g] << " ended -> key_release " << keys[k]
           << "\n";
      k = (k + 1) % 9;
    }
  }

  const BindingSet original = MustParse(text.str());
  const std::string serialized = SerializeBindings(original);

  BindingSet reparsed;
  std::string error;
  ASSERT_TRUE(ParseBindings(serialized, reparsed, error))
      << error << "\n" << serialized;
  ASSERT_EQ(reparsed.bindings.size(), original.bindings.size());
  for (std::size_t i = 0; i < original.bindings.size(); ++i) {
    EXPECT_EQ(original.bindings[i].gesture, reparsed.bindings[i].gesture);
    EXPECT_EQ(original.bindings[i].phase, reparsed.bindings[i].phase);
    EXPECT_EQ(original.bindings[i].hand, reparsed.bindings[i].hand);
    EXPECT_TRUE(SameCommand(original.bindings[i].command,
                            reparsed.bindings[i].command))
        << "binding " << i;
  }
}

// A second pass must produce byte-identical text. If it does not, the writer
// is not a fixed point and repeated saves from a UI would churn the file.
TEST(Bindings, SerializeIsAFixedPoint) {
  const BindingSet once = MustParse(DefaultBindingsText());
  const std::string first = SerializeBindings(once);
  const BindingSet twice = MustParse(first);
  EXPECT_EQ(SerializeBindings(twice), first);
}

TEST(Bindings, SerializedSettingsSurviveAwkwardFloats) {
  // 0.1 is not representable in binary floating point; a writer emitting too
  // few significant digits loses it.
  const BindingSet set = MustParse(
      "set sensitivity 0.1\n"
      "set smoothing 0.3333333\n" +
      std::string(kMinimal));
  BindingSet reparsed;
  std::string error;
  ASSERT_TRUE(ParseBindings(SerializeBindings(set), reparsed, error)) << error;
  EXPECT_FLOAT_EQ(reparsed.sensitivity, set.sensitivity);
  EXPECT_FLOAT_EQ(reparsed.smoothing, set.smoothing);
}

// --- empty input ----------------------------------------------------------

// An empty configuration is valid and means "no bindings, default settings".
// It is not the same thing as a broken one, and the distinction matters for a
// UI that starts a user from blank.
TEST(Bindings, EmptyTextIsValidAndYieldsDefaults) {
  const BindingSet set = MustParse("");
  EXPECT_TRUE(set.bindings.empty());
  EXPECT_FLOAT_EQ(set.sensitivity, hc::kDefaultSensitivity);
  EXPECT_FLOAT_EQ(set.smoothing, hc::kDefaultSmoothing);
}

TEST(Bindings, ParseClearsPreviousContentOnSuccess) {
  BindingSet set = MustParse(DefaultBindingsText());
  ASSERT_FALSE(set.bindings.empty());

  std::string error;
  ASSERT_TRUE(ParseBindings(kMinimal, set, error)) << error;
  EXPECT_EQ(set.bindings.size(), 1u)
      << "a reparse must replace the old bindings, not append to them";
}
