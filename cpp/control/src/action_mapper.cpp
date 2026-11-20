#include "hc/action_mapper.hpp"

#include <utility>

namespace hc {

ActionMapper::ActionMapper(BindingSet bindings)
    : bindings_(std::move(bindings)), tracks_{} {
  for (TrackState& t : tracks_) t.in_use = false;
}

void ActionMapper::SetBindings(BindingSet bindings) {
  bindings_ = std::move(bindings);
  Reset();
}

void ActionMapper::Reset() {
  for (TrackState& t : tracks_) t.in_use = false;
}

ActionMapper::TrackState* ActionMapper::FindOrCreate(
    std::uint32_t track_id) {
  for (TrackState& t : tracks_) {
    if (t.in_use && t.track_id == track_id) {
      t.touched = true;
      return &t;
    }
  }
  TrackState* slot = nullptr;
  for (TrackState& t : tracks_) {
    if (!t.in_use) {
      slot = &t;
      break;
    }
  }
  if (slot == nullptr) {
    // Table full. The vision tracker issues a new id on every re-detection, so
    // a long session leaves slots held by tracks that are long gone. Reclaim
    // one whose track did not appear in this Map call; a slot touched in this
    // call belongs to a live track and is never taken.
    for (TrackState& t : tracks_) {
      if (!t.touched) {
        slot = &t;
        break;
      }
    }
  }
  if (slot == nullptr) return nullptr;  // Every slot is live. See Filter.
  slot->track_id = track_id;
  slot->in_use = true;
  slot->touched = true;
  slot->smoothed = Point2{0.0f, 0.0f};
  return slot;
}

void ActionMapper::FreeTrack(std::uint32_t track_id) {
  for (TrackState& t : tracks_) {
    if (t.in_use && t.track_id == track_id) t.in_use = false;
  }
}

void ActionMapper::ResetTrack(std::uint32_t track_id) {
  for (TrackState& t : tracks_) {
    if (t.in_use && t.track_id == track_id) {
      t.smoothed = Point2{0.0f, 0.0f};
      return;
    }
  }
}

Point2 ActionMapper::Filter(std::uint32_t track_id, const Point2& raw) {
  const float a = bindings_.smoothing;
  const float s = bindings_.sensitivity;

  TrackState* state = FindOrCreate(track_id);
  if (state == nullptr) {
    // More tracks than the table holds. Degrade to UNFILTERED motion rather
    // than dropping the command: a slightly jittery cursor is a far smaller
    // fault than a click that never happens because an implausible number of
    // hands were in frame.
    return Point2{raw.x * s, raw.y * s};
  }

  state->smoothed = Point2{a * state->smoothed.x + (1.0f - a) * raw.x,
                           a * state->smoothed.y + (1.0f - a) * raw.y};
  return Point2{state->smoothed.x * s, state->smoothed.y * s};
}

void ActionMapper::Map(const std::vector<GestureEvent>& events,
                       std::vector<Command>& out) {
  MapInto(events, out, nullptr);
}

void ActionMapper::Map(const std::vector<GestureEvent>& events,
                       std::vector<Command>& out,
                       std::vector<std::uint32_t>& out_origin) {
  MapInto(events, out, &out_origin);
}

void ActionMapper::MapInto(const std::vector<GestureEvent>& events,
                           std::vector<Command>& out,
                           std::vector<std::uint32_t>* out_origin) {
  out.clear();
  if (out_origin != nullptr) out_origin->clear();

  for (TrackState& t : tracks_) t.touched = false;

  for (std::size_t index = 0; index < events.size(); ++index) {
    const GestureEvent& e = events[index];
    // A new gesture starts from rest. Without this the tail of the previous
    // gesture's motion would leak into the new one -- a quick pinch followed
    // by pointing would fling the cursor.
    if (e.phase == GestureEvent::Phase::Began) ResetTrack(e.track_id);

    for (const Binding& b : bindings_.bindings) {
      if (b.gesture != e.kind) continue;
      if (b.phase != e.phase) continue;
      if (!HandSelectorMatches(b.hand, e.handedness)) continue;

      Command c{};
      c.kind = b.command.kind;

      switch (b.command.kind) {
        case Command::Kind::MoveCursorRelative:
        case Command::Kind::Scroll:
          // Both are continuous motion derived from the palm centre, so both
          // get the same sensitivity and the same jitter filter. Leaving
          // scroll raw would make it the one motion a user could not tune.
          c.delta = Filter(e.track_id, e.delta);
          break;

        case Command::Kind::Click:
        case Command::Kind::DoubleClick:
          // delta stays zero. A click carrying the cursor's last movement
          // would make a sink that reads delta unconditionally drag the
          // pointer mid-click.
          c.button = b.command.button;
          break;

        case Command::Kind::KeyPress:
        case Command::Kind::KeyRelease:
          c.key = b.command.key;
          break;

        case Command::Kind::None:
          continue;  // ParseBindings rejects this; a hand-built set may not.
      }

      out.push_back(c);
      if (out_origin != nullptr) {
        out_origin->push_back(static_cast<std::uint32_t>(index));
      }
    }

    // The track's gesture is over, so its smoothing slot is too. Freed after
    // its bindings ran, so an Ended-bound motion command still filtered. A
    // returning track starts from rest anyway (Began resets).
    if (e.phase == GestureEvent::Phase::Ended) FreeTrack(e.track_id);
  }
}

}  // namespace hc
