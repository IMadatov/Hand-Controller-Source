#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/gesture_recognizer.hpp"

namespace hc {

// Turns gesture events into abstract commands, according to configuration.
//
// PURE, and in the same sense the recognizer is: no clock, no OS, no OpenCV,
// no screen dimensions. It is the last layer that thinks in frame-normalized
// 0..1 coordinates, and it must stay that way -- OutputSink is the one place
// permitted to multiply by pixels, and it is the second and last such place
// in the whole project.
//
// If this class ever acquires a screen width, the layering has been broken:
// the mapper would then produce different output on different monitors for
// the same hand movement, and the same configuration would stop meaning the
// same thing on two machines.
class ActionMapper {
 public:
  explicit ActionMapper(BindingSet bindings);

  // Appends a command for every event matching a binding, to `out`, which is
  // CLEARED first. `out` is caller-owned so a frame loop can reuse capacity.
  //
  // An event matching no binding produces nothing. That is the normal case,
  // not an error: a user who binds three gestures still makes the other six.
  //
  // Normally at most one command per event, since ParseBindings rejects
  // overlapping bindings. A BindingSet assembled in code can still contain
  // overlaps, and this function emits one command per match rather than
  // silently picking one -- it reports what the configuration says, and the
  // parser is where a configuration is judged.
  void Map(const std::vector<GestureEvent>& events,
           std::vector<Command>& out);

  // The same, plus PROVENANCE: `out_origin[i]` is the index into `events` of
  // the event that produced `out[i]`. Both vectors are cleared first and end
  // the same length.
  //
  // Exists because a command alone cannot explain itself. A UI showing a
  // command log needs to say which hand and which gesture caused each entry,
  // and a Command deliberately does not carry that -- it is a plain-data
  // instruction to a sink, and growing it with fields only a log reads would
  // put UI concerns on the injection path. Keeping the link as a parallel
  // index means the sink's input stays exactly what it was.
  void Map(const std::vector<GestureEvent>& events, std::vector<Command>& out,
           std::vector<std::uint32_t>& out_origin);

  // Replaces the configuration and drops all smoothing state.
  //
  // Exists because bindings are editable at runtime -- `--bindings` reloading
  // and, later, the bindings UI applying a change without a restart. Carrying
  // the old smoothing state across a sensitivity change would make the first
  // few frames after the edit behave like neither setting.
  void SetBindings(BindingSet bindings);

  const BindingSet& Bindings() const { return bindings_; }

  // Drops smoothing state for every track without touching the bindings.
  void Reset();

 private:
  // Smoothing is per track_id, never shared. Two hands moving independently
  // must not blend into each other, which is the same reason the recognizer
  // keys its state this way. Fixed capacity for the same reason too: Map runs
  // every frame.
  struct TrackState {
    std::uint32_t track_id;
    bool in_use;
    Point2 smoothed;  // Last smoothed motion for this track.
    bool touched;     // Used during the current Map call; not evictable.
  };

  // The one implementation behind both Map overloads. `out_origin` is null
  // when the caller does not want provenance.
  void MapInto(const std::vector<GestureEvent>& events,
               std::vector<Command>& out,
               std::vector<std::uint32_t>* out_origin);

  TrackState* FindOrCreate(std::uint32_t track_id);
  void FreeTrack(std::uint32_t track_id);
  void ResetTrack(std::uint32_t track_id);

  // Applies sensitivity and exponential smoothing to one frame of motion.
  //
  //   smoothed = smoothing * previous + (1 - smoothing) * raw
  //
  // so smoothing 0 is exact pass-through and smoothing 1 would freeze the
  // output entirely. Landmark jitter is real and one bad frame must not throw
  // the cursor across the screen; this is the filter that stops it.
  //
  // Smoothing lives here rather than in the recognizer on purpose: the
  // recognizer reports what the hand did, and how hard to filter that for
  // comfort is a per-user binding concern.
  Point2 Filter(std::uint32_t track_id, const Point2& raw);

  BindingSet bindings_;
  std::array<TrackState, kMaxGestureTracks> tracks_;
};

}  // namespace hc
