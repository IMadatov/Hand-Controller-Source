#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hc/gesture_event.hpp"
#include "hc/hand_features.hpp"
#include "hc/hand_observation.hpp"

namespace hc {

// The two thresholds that give one gesture its hysteresis, in SCORE space:
// every gesture is reduced to a match score in [0, 1] (see GestureScore), so
// one pair of numbers configures any of them.
//
// `enter` must be strictly greater than `exit`. That gap is the hysteresis,
// and it is the only thing standing between a pose hovering at its threshold
// and a click firing thirty times a second. Equal thresholds would restore
// the chatter; `enter` below `exit` would latch forever.
struct GestureThresholds {
  float enter;
  float exit;
};

// Tuning for the recognizer. Plain data, so a later task can load it from the
// same configuration file as the bindings without this header changing.
struct GestureConfig {
  // Indexed by GestureKind. The GestureKind::None slot is never read.
  std::array<GestureThresholds, kGestureKindCount> thresholds;

  // The pinch_distance (in palm-size units, as HandFeatures reports it) at
  // which the pinch score reaches zero. Distances at or above this score 0,
  // a distance of 0 scores 1, and the range between is linear.
  //
  // This is what makes pinch the one gesture with a genuinely continuous
  // score: the others are built from the five extended-finger booleans and
  // can only move in fifths.
  float pinch_span;

  // How many CONSECUTIVE frames a better-scoring gesture must win before it
  // takes over from the active one. At least 1.
  //
  // Why a takeover exists at all: pattern scores move in fifths, and the exit
  // threshold tolerates one finger of disagreement so a flicker does not end
  // a gesture. But five pairs in the vocabulary differ by exactly one finger
  // -- fist/point_index, fist/thumb_up, fist/pinky_only,
  // open_palm/four_fingers and point_index/two_fingers -- so that same
  // tolerance makes the transition between them IMPOSSIBLE without it. The
  // last pair is the default configuration's primary interaction: point to
  // move the cursor, raise the middle finger to click.
  //
  // Why it is confirmed over several frames rather than instantly: a takeover
  // ends one gesture and begins another, and a one-shot binding fires on that
  // begin. A middle finger wobbling between two bound poses would otherwise
  // produce a click per wobble, which is the chatter the hysteresis exists to
  // prevent. Two frames is roughly 20 ms at the measured frame rate -- below
  // human perception, and enough to reject a single bad frame of landmarks.
  int steal_confirm_frames;

  // The defaults the application uses. Chosen, not measured -- there is no
  // camera on the build machine, so these are starting points for the
  // owner to tune against a real hand, not findings.
  static GestureConfig Default();
};

// Returns how strongly `f` matches `kind`, in [0, 1]. Exposed because it is
// the whole of the pose-to-gesture decision and is worth testing directly,
// separately from the latching around it.
//
// GestureKind::None always scores 0.
//
// For the five pattern gestures (Fist, OpenPalm, PointIndex,
// TwoFingerScroll, ThumbUp) the score is the fraction of the five fingers
// whose extended/curled state matches the gesture's target pattern. So the
// score moves in steps of 0.2, and a default enter of 0.9 with an exit of
// 0.7 means: all five fingers must agree to START the gesture, but one
// finger may flicker without ENDING it. That asymmetry is deliberate.
float GestureScore(GestureKind kind, const HandFeatures& f,
                   const GestureConfig& config);

// The most tracks the recognizer keeps state for in one session. Matches the
// 32-hand ceiling the CLI enforces on --max-hands; past this, the oldest
// state is not silently reused, the extra track is ignored.
inline constexpr std::size_t kMaxGestureTracks = 32;

// Turns a stream of per-frame hand observations into Began/Held/Ended gesture
// events, with one independent state machine per `track_id`.
//
// PURE. No clock, no I/O, no OS, no OpenCV. Every timestamp arrives as an
// argument and is passed through untouched. A test for this class needs no
// camera and no display; if one does, the design has gone wrong.
//
// The three rules that define the behaviour, each answering a defect in the
// Python version:
//
//   1. ONE gesture is active per track at a time, and it stays active until
//      its own score falls below its own exit threshold, or until a different
//      gesture clearing its own enter threshold and scoring strictly higher
//      has done so for `steal_confirm_frames` consecutive frames. Without the
//      first half, a fist and a pinch both matching would emit two interleaved
//      streams; without the second, the five one-finger-apart pairs could
//      never transition at all. A takeover emits `Ended` for the old gesture
//      and the new one begins on a LATER frame, never the same one.
//
//   2. State lives per `track_id` and nowhere else. A track that disappears
//      invalidates only its own state (spec defect 4: shared globals kept
//      firing for a hand that had left).
//
//   3. `Began` fires once per gesture, not once per frame the pose holds
//      (spec defect 5: the Python latch reset every frame).
class GestureRecognizer {
 public:
  explicit GestureRecognizer(const GestureConfig& config);

  // Advances every track's state machine by one frame and appends the
  // resulting events to `out`, which is CLEARED first.
  //
  // `hands` is the complete set of hands seen this frame. A track with state
  // here but absent from `hands` has left the frame: it emits `Ended` for
  // whatever it was holding and its state is dropped. An empty `hands` is a
  // normal frame, not an error (spec defect 2) -- it ends every active
  // gesture and emits nothing further.
  //
  // `out` is a caller-owned buffer so a frame loop can reuse its capacity;
  // this function performs no allocation of its own beyond growing `out`.
  //
  // At most one event per track per frame. When a gesture ends because the
  // pose relaxed, the `Ended` is emitted on that frame and any new gesture
  // begins on a later one -- a release and an unrelated press never arrive
  // from the same frame.
  void Process(const std::vector<HandObservation>& hands,
               std::int64_t timestamp_us, std::vector<GestureEvent>& out);

  // Forgets `track_id` WITHOUT emitting anything.
  //
  // This is for the caller that already knows the track is gone and has
  // handled the consequences itself. It is not the normal path: letting the
  // track fall out of `hands` is, because that emits the `Ended` a binding
  // needs in order to release a held key. Calling this on a track holding a
  // gesture will strand that key down, so prefer omission over invalidation.
  void InvalidateTrack(std::uint32_t track_id);

  // Drops all state, emitting nothing. Same caveat as InvalidateTrack.
  void Reset();

 private:
  // One track's state. `active` is GestureKind::None when the track is seen
  // but holding nothing, which is distinct from the track being absent.
  struct TrackState {
    std::uint32_t track_id;
    bool in_use;
    GestureKind active;
    Handedness latched_handedness;  // Fixed at Began. See GestureEvent.

    // Palm centre on this track's previous frame, and whether there was one.
    // `has_position` is false for exactly one frame per track, and without it
    // a newly seen hand would report a delta measured from the origin.
    Point2 last_position;
    bool has_position;

    // The gesture currently contesting `active`, and for how many CONSECUTIVE
    // frames it has out-scored it. `challenger_frames` resets to zero the
    // moment no gesture contests, so a part-won takeover is abandoned rather
    // than resumed later. Both are meaningless while `active` is None.
    GestureKind challenger;
    int challenger_frames;

    bool seen_this_frame;
  };

  TrackState* Find(std::uint32_t track_id);
  TrackState* FindOrCreate(std::uint32_t track_id);

  GestureConfig config_;

  // Fixed capacity rather than a map: Process runs on every frame, and the
  // project's measured per-frame budget is around 11 ms for two hands. A
  // node-based container would allocate on every newly seen track and spread
  // this state across the heap for the sake of a lookup over at most 32
  // entries. The linear scan is the faster structure at this size.
  std::array<TrackState, kMaxGestureTracks> tracks_;
};

}  // namespace hc
