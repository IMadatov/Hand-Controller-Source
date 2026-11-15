#pragma once

#include <cstddef>
#include <cstdint>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"

namespace hc {

// The gesture vocabulary. Every binding in the configuration names one of
// these, so adding a kind here is the only place a new gesture has to be
// declared.
//
// `None` is not a gesture and is never emitted in a GestureEvent. It exists
// so that "no gesture is active" is representable without a sentinel or an
// optional, and it occupies index 0 so the threshold table below can be
// indexed directly by the enum.
// Each name describes the POSE, never the action it is expected to drive.
// Bindings are configuration, so any of these can be bound to anything; a
// kind called "TwoFingerScroll" would be a lie the moment a user bound it to
// a click, which is in fact what the default configuration does.
enum class GestureKind {
  None = 0,
  Pinch,        // Thumb and index TIPS together. Scored by distance.
  Fist,         // No finger extended.
  OpenPalm,     // All five extended.
  PointIndex,   // Index only.
  TwoFingers,   // Index and middle.
  ThumbUp,      // Thumb only.
  PinkyOnly,    // Pinky only.
  FourFingers,  // All four fingers, thumb folded.
};

// The number of entries in GestureKind, `None` included. Used to size the
// per-kind threshold table in GestureConfig.
//
// Keep this in step with the enum. A kind added above without bumping this
// would index past the table, which is why GestureConfig asserts its own
// table size rather than trusting the number.
inline constexpr std::size_t kGestureKindCount = 9;

// Returns a stable, lowercase name for `kind`: "pinch", "fist", "open_palm",
// "point_index", "two_fingers", "thumb_up", "pinky_only", "four_fingers",
// "none".
//
// These strings are a published interface -- the bindings file uses them --
// so they must not be reworded once configurations exist in the wild.
const char* GestureKindName(GestureKind kind);

// Parses a name produced by GestureKindName. Returns false if `name` matches
// none of them, leaving `out` untouched.
//
// "none" IS accepted here, because this function's job is to mirror
// GestureKindName exactly. Rejecting it as a binding target is the bindings
// parser's business, not this function's.
bool GestureKindFromName(const char* name, GestureKind& out);

// One gesture transition for one tracked hand on one frame.
//
// The three phases are the whole point of this type, and the reason the
// recognizer does not simply report a level ("fist is true this frame"):
//
//   Began  the gesture just became active. A one-shot binding (click,
//          screenshot) fires here, exactly once.
//   Held   the gesture is still active. A continuous binding (cursor motion,
//          scroll) acts here, every frame.
//   Ended  the gesture stopped, either because the pose relaxed or because
//          the hand left the frame. A binding holding a key or a mouse
//          button releases here.
//
// A design emitting only a level cannot express the one-shot latch or the
// release, which are spec defects 5 and 7 respectively.
struct GestureEvent {
  enum class Phase { Began, Held, Ended };

  std::uint32_t track_id;

  // The handedness LATCHED AT `Began`, not re-read per frame.
  //
  // The landmark model computes handedness per crop and it can flip mid-track
  // -- phase 1 measured confidence as low as 0.575 on a two-hand frame. If
  // this field followed the current observation, a flip halfway through a
  // held gesture would hand the event to the other hand's binding and a
  // left-hand gesture would start firing right-hand commands. Identity
  // follows `track_id`; this label is fixed for the life of the gesture.
  Handedness handedness;

  GestureKind kind;
  Phase phase;

  // The hand's palm centre, frame-normalized like every coordinate below the
  // platform layer. On an `Ended` caused by the hand leaving the frame this
  // is the last position observed, since there is no current one.
  Point2 position;

  // Movement of `position` since the previous frame in which this track was
  // seen. Zero on `Began`, and zero on an `Ended` caused by the hand leaving
  // the frame.
  //
  // Relative rather than absolute on purpose: the mapper multiplies this by a
  // sensitivity factor, which is what lets a small hand movement cross a wide
  // display. The Python version's absolute 1:1 mapping could not.
  Point2 delta;

  // Passed straight through from the caller's frame timestamp. The recognizer
  // never reads a clock and never compares two timestamps, so a non-monotonic
  // sequence is carried faithfully rather than being corrected or rejected.
  std::int64_t timestamp_us;
};

}  // namespace hc
