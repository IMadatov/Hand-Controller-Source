#pragma once

#include <string>
#include <vector>

#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/hand_observation.hpp"

namespace hc {

// Which hand a binding applies to.
//
// This is NOT `Handedness`, and the difference matters. Handedness has three
// values and the third is `Unknown`, meaning "the model could not tell" --
// which is a description of a hand, not a thing a user would ever write in a
// configuration file. Phase 1 measured handedness confidence as low as 0.575,
// so `Unknown` arrives often; with only Left and Right available to bind, an
// unlabelled hand would match nothing and every gesture it made would
// silently do nothing.
//
// `Any` is the fix, and it is why this type exists. It matches Left, Right
// and Unknown alike.
enum class HandSelector { Any, Left, Right };

// "any", "left", "right".
const char* HandSelectorName(HandSelector hand);
bool HandSelectorFromName(const char* name, HandSelector& out);

// True when a gesture event from a hand labelled `handedness` should be
// matched by a binding written for `selector`.
bool HandSelectorMatches(HandSelector selector, Handedness handedness);

// One line of the configuration: a gesture in a phase, on a hand, does this.
struct Binding {
  HandSelector hand;
  GestureKind gesture;
  GestureEvent::Phase phase;
  Command command;
};

// Defaults applied when a configuration does not say otherwise.
//
// BOTH ARE CHOSEN, NOT MEASURED. There is no camera on the build machine, so
// no value here was arrived at by watching a cursor move. They are starting
// points for the owner to tune, and the only reason they are not zero is that
// zero would be unusable.
inline constexpr float kDefaultSensitivity = 2.0f;
inline constexpr float kDefaultSmoothing = 0.5f;

struct BindingSet {
  std::vector<Binding> bindings;

  // Multiplies normalized cursor motion. Above 1 so a small hand movement can
  // cross a wide display -- the thing the Python version's 1:1 mapping could
  // not do. Must be strictly positive.
  float sensitivity = kDefaultSensitivity;

  // Exponential smoothing applied to cursor motion by the mapper, in [0, 1].
  // 0 is no smoothing; 1 would be infinite smoothing, i.e. a cursor that
  // never moves, which is why the range is closed at both ends but a sane
  // value sits in the middle.
  float smoothing = kDefaultSmoothing;
};

// Parses the bindings file format. Returns true on success. On failure
// returns false, writes a message naming the problem AND THE LINE NUMBER to
// `error`, and leaves `out` in an unspecified state.
//
// A configuration error the user cannot locate is a configuration format that
// failed, so every message carries `line N:`.
//
// THE FORMAT, which is flat and line-oriented rather than JSON or YAML: JSON
// would mean a fetched dependency, which the project's "a clean clone plus
// cmake is enough" rule forbids, and hand-rolling one is more code than every
// layer it would configure put together.
//
//   # comment, and blank lines, are ignored
//   set sensitivity 2.0
//   set smoothing 0.5
//   <hand> <gesture> <phase> -> <command> [arg]
//
// where <hand> is any|left|right, <gesture> is a GestureKindName other than
// "none", <phase> is began|held|ended, and <command> is a CommandKindName
// other than "none". `click` and `double_click` take an optional button name
// and default to left; `key_press` and `key_release` REQUIRE a key name;
// `move_cursor` and `scroll` take no argument and reject one.
//
// Two rules are enforced that a looser parser would let through silently:
//
//   1. NO TWO BINDINGS MAY OVERLAP on the same gesture and phase. Exact
//      duplicates are the obvious case, but `any` overlapping a `left` for
//      the same gesture and phase is the subtle one -- a left hand would then
//      fire two commands from one gesture. Letting the last line win makes a
//      typo invisible, which is the failure this rule exists to prevent.
//
//   2. EVERY `key_press` MUST HAVE A MATCHING `key_release` for the same hand,
//      gesture and key, on the `ended` phase. A press without a release is a
//      key held down after the gesture stops and after the program exits --
//      the plan calls a stuck modifier the most likely way this build hurts
//      someone, and a configuration is the easiest place to cause one.
//      Checked here so it fails at load, not after the damage.
bool ParseBindings(const std::string& text, BindingSet& out,
                   std::string& error);

// Writes `set` back out in the format ParseBindings reads, such that
// ParseBindings(SerializeBindings(x)) yields x again.
//
// This exists because the eventual Commands & Bindings UI is there to EDIT
// bindings and save them. Without a writer in this layer, that UI would
// reimplement the format, and the two definitions would drift the first time
// either changed. The round-trip property is pinned by a test rather than
// assumed.
std::string SerializeBindings(const BindingSet& set);

// The configuration shipped in `config/default_bindings.txt`, as a string.
//
// Compiled in rather than read from disk so that the application has
// something working with no file present, and so the test that the shipped
// default parses cleanly does not depend on a path. The file and this string
// are kept identical by a test.
const char* DefaultBindingsText();

}  // namespace hc
