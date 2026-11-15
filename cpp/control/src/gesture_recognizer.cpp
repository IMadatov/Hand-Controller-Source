#include "hc/gesture_recognizer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace hc {
namespace {

std::size_t Index(GestureKind kind) {
  return static_cast<std::size_t>(kind);
}

float Clamp01(float v) {
  return std::min(1.0f, std::max(0.0f, v));
}

// The target extended/curled pattern of each pattern gesture, thumb first.
//
// `has_pattern` is false for the two kinds that are not decided this way:
// None, which is not a gesture, and Pinch, which is scored from a continuous
// distance instead.
struct PatternRule {
  bool has_pattern;
  bool want[5];
};

PatternRule RuleFor(GestureKind kind) {
  switch (kind) {
    case GestureKind::Fist:
      return {true, {false, false, false, false, false}};
    case GestureKind::OpenPalm:
      return {true, {true, true, true, true, true}};
    case GestureKind::PointIndex:
      return {true, {false, true, false, false, false}};
    case GestureKind::TwoFingers:
      return {true, {false, true, true, false, false}};
    case GestureKind::ThumbUp:
      return {true, {true, false, false, false, false}};
    case GestureKind::PinkyOnly:
      return {true, {false, false, false, false, true}};
    case GestureKind::FourFingers:
      return {true, {false, true, true, true, true}};
    case GestureKind::Pinch:
    case GestureKind::None:
      break;
  }
  return {false, {false, false, false, false, false}};
}

}  // namespace

GestureConfig GestureConfig::Default() {
  GestureConfig c{};

  // A thumb-to-index distance of one palm-size scores zero. Picked so that
  // the interesting range -- tips touching, up to tips a finger apart -- maps
  // onto most of 0..1 rather than being squeezed into one end.
  c.pinch_span = 1.0f;

  // None is unreachable by construction: GestureScore returns 0 for it and no
  // threshold here is at or below 0. These values exist so that the slot is
  // never accidentally meaningful.
  c.thresholds[Index(GestureKind::None)] = {2.0f, 1.0f};

  // Pinch, the one continuous gesture. With pinch_span at 1.0 these mean:
  // close the pinch at 0.25 palm-sizes between the tips, release it past
  // 0.40. The 0.15 gap is the hysteresis, and it is wide because this is the
  // gesture most likely to be bound to a click -- the one place where chatter
  // is not cosmetic.
  c.thresholds[Index(GestureKind::Pinch)] = {0.75f, 0.60f};

  // The pattern gestures, whose scores move in fifths. Enter at 0.9 means all
  // five fingers must agree to START; exit at 0.7 means a single finger may
  // flicker (4/5 = 0.8) without ENDING it, while two disagreeing (3/5 = 0.6)
  // do end it. That asymmetry is the whole hysteresis for these kinds.
  const GestureThresholds pattern{0.90f, 0.70f};
  c.thresholds[Index(GestureKind::Fist)] = pattern;
  c.thresholds[Index(GestureKind::OpenPalm)] = pattern;
  c.thresholds[Index(GestureKind::PointIndex)] = pattern;
  c.thresholds[Index(GestureKind::TwoFingers)] = pattern;
  c.thresholds[Index(GestureKind::ThumbUp)] = pattern;
  c.thresholds[Index(GestureKind::PinkyOnly)] = pattern;
  c.thresholds[Index(GestureKind::FourFingers)] = pattern;

  // Two frames is roughly 20 ms at the measured frame rate: below human
  // perception, and enough to reject a single bad frame of landmarks. See the
  // field's comment for why a takeover exists at all.
  c.steal_confirm_frames = 2;

  return c;
}

const char* GestureKindName(GestureKind kind) {
  switch (kind) {
    case GestureKind::None: return "none";
    case GestureKind::Pinch: return "pinch";
    case GestureKind::Fist: return "fist";
    case GestureKind::OpenPalm: return "open_palm";
    case GestureKind::PointIndex: return "point_index";
    case GestureKind::TwoFingers: return "two_fingers";
    case GestureKind::ThumbUp: return "thumb_up";
    case GestureKind::PinkyOnly: return "pinky_only";
    case GestureKind::FourFingers: return "four_fingers";
  }
  return "none";
}

bool GestureKindFromName(const char* name, GestureKind& out) {
  if (name == nullptr) return false;
  for (std::size_t i = 0; i < kGestureKindCount; ++i) {
    const GestureKind kind = static_cast<GestureKind>(i);
    if (std::strcmp(name, GestureKindName(kind)) == 0) {
      out = kind;
      return true;
    }
  }
  return false;
}

float GestureScore(GestureKind kind, const HandFeatures& f,
                   const GestureConfig& config) {
  if (kind == GestureKind::None) return 0.0f;

  if (kind == GestureKind::Pinch) {
    // pinch_span is a divisor. HandFeatures guarantees palm_size positive but
    // says nothing about this, which arrives from configuration.
    if (!(config.pinch_span > 0.0f)) return 0.0f;
    return Clamp01(1.0f - f.pinch_distance / config.pinch_span);
  }

  const PatternRule rule = RuleFor(kind);
  if (!rule.has_pattern) return 0.0f;

  int matches = 0;
  for (int i = 0; i < 5; ++i) {
    if (f.extended[i] == rule.want[i]) ++matches;
  }
  return static_cast<float>(matches) / 5.0f;
}

GestureRecognizer::GestureRecognizer(const GestureConfig& config)
    : config_(config), tracks_{} {
  // The hysteresis gap is the one invariant the whole class rests on: without
  // it a pose resting at its threshold emits a Began on every other frame.
  // Checked here rather than tolerated, because a config that fails this is a
  // programming error, not user input -- Task 5's file parser is where a
  // user-supplied value gets a readable message instead.
  for (std::size_t i = 0; i < kGestureKindCount; ++i) {
    assert(config_.thresholds[i].enter > config_.thresholds[i].exit &&
           "GestureConfig: enter threshold must be strictly above exit");
  }
  assert(config_.pinch_span > 0.0f && "GestureConfig: pinch_span must be > 0");
  assert(config_.steal_confirm_frames >= 1 &&
         "GestureConfig: steal_confirm_frames must be at least 1");

  // Floored rather than trusted, because the asserts above vanish under
  // NDEBUG and a count of zero there would mean an INSTANT takeover -- the one
  // behaviour the confirmation window exists to prevent.
  config_.steal_confirm_frames = std::max(1, config_.steal_confirm_frames);

  for (TrackState& t : tracks_) t.in_use = false;
}

GestureRecognizer::TrackState* GestureRecognizer::Find(
    std::uint32_t track_id) {
  for (TrackState& t : tracks_) {
    if (t.in_use && t.track_id == track_id) return &t;
  }
  return nullptr;
}

GestureRecognizer::TrackState* GestureRecognizer::FindOrCreate(
    std::uint32_t track_id) {
  if (TrackState* existing = Find(track_id)) return existing;

  for (TrackState& t : tracks_) {
    if (t.in_use) continue;
    t.track_id = track_id;
    t.in_use = true;
    t.active = GestureKind::None;
    t.latched_handedness = Handedness::Unknown;
    t.last_position = Point2{0.0f, 0.0f};
    t.has_position = false;
    t.challenger = GestureKind::None;
    t.challenger_frames = 0;
    t.seen_this_frame = false;
    return &t;
  }

  // At capacity. The extra track is IGNORED rather than given a recycled
  // slot: evicting a live track would retarget its held gesture onto a
  // different hand, which is worse than not tracking the surplus one.
  return nullptr;
}

void GestureRecognizer::Process(const std::vector<HandObservation>& hands,
                                std::int64_t timestamp_us,
                                std::vector<GestureEvent>& out) {
  out.clear();

  for (TrackState& t : tracks_) t.seen_this_frame = false;

  for (const HandObservation& hand : hands) {
    TrackState* state = FindOrCreate(hand.track_id);
    if (state == nullptr) continue;  // At capacity; see FindOrCreate.
    state->seen_this_frame = true;

    const HandFeatures f = ComputeFeatures(hand);

    // Movement since this track's previous frame, which is not the same thing
    // as movement since the gesture began -- a hand can be tracked for a
    // while before any gesture latches.
    const Point2 motion =
        state->has_position
            ? Point2{f.palm_center.x - state->last_position.x,
                     f.palm_center.y - state->last_position.y}
            : Point2{0.0f, 0.0f};
    state->last_position = f.palm_center;
    state->has_position = true;

    GestureEvent ev{};
    ev.track_id = hand.track_id;
    ev.position = f.palm_center;
    ev.timestamp_us = timestamp_us;

    if (state->active == GestureKind::None) {
      // Nothing held: look for a gesture clearing its enter threshold, and
      // take the highest-scoring one. Ties go to the earlier enum entry,
      // which makes the choice deterministic rather than merely consistent
      // within one build.
      GestureKind best = GestureKind::None;
      float best_score = 0.0f;
      for (std::size_t i = 1; i < kGestureKindCount; ++i) {
        const GestureKind kind = static_cast<GestureKind>(i);
        const float score = GestureScore(kind, f, config_);
        if (score >= config_.thresholds[i].enter && score > best_score) {
          best = kind;
          best_score = score;
        }
      }
      if (best == GestureKind::None) continue;  // Nothing to report.

      state->active = best;
      state->latched_handedness = hand.handedness;
      state->challenger = GestureKind::None;
      state->challenger_frames = 0;

      ev.kind = best;
      ev.phase = GestureEvent::Phase::Began;
      ev.handedness = state->latched_handedness;
      ev.delta = Point2{0.0f, 0.0f};  // No motion belongs to a gesture's start.
      out.push_back(ev);
      continue;
    }

    // Something is held. It keeps running on ITS OWN exit threshold, and a
    // single frame of another gesture scoring higher does not steal it --
    // without that, an ambiguous pose would alternate between two kinds,
    // emitting a Began and an Ended every frame.
    const std::size_t active_index = Index(state->active);
    const float score = GestureScore(state->active, f, config_);

    ev.kind = state->active;
    ev.handedness = state->latched_handedness;  // Latched, never re-read.
    ev.delta = motion;

    if (score < config_.thresholds[active_index].exit) {
      // The pose relaxed out of the gesture on its own terms. No takeover is
      // involved, so the challenger is irrelevant and is dropped with it.
      ev.phase = GestureEvent::Phase::Ended;
      state->active = GestureKind::None;
      state->challenger = GestureKind::None;
      state->challenger_frames = 0;
      out.push_back(ev);
      continue;
    }

    // The active gesture still clears its exit threshold, so the only way out
    // of it this frame is a TAKEOVER: a different gesture clearing its own
    // enter threshold AND scoring strictly higher, for steal_confirm_frames
    // consecutive frames. Needed because five pairs in the vocabulary differ
    // by exactly one finger, which the exit threshold deliberately tolerates
    // -- so point_index would otherwise hold through a two_fingers pose
    // forever, and the default configuration's click could never fire.
    GestureKind challenger = GestureKind::None;
    float challenger_score = score;
    for (std::size_t i = 1; i < kGestureKindCount; ++i) {
      if (static_cast<GestureKind>(i) == state->active) continue;
      const GestureKind kind = static_cast<GestureKind>(i);
      const float other = GestureScore(kind, f, config_);
      if (other >= config_.thresholds[i].enter && other > challenger_score) {
        challenger = kind;
        challenger_score = other;
      }
    }

    if (challenger == GestureKind::None) {
      // Nothing is contesting it, so a part-won takeover is abandoned rather
      // than resumed later: the frames must be CONSECUTIVE.
      state->challenger_frames = 0;
    } else if (challenger == state->challenger) {
      ++state->challenger_frames;
    } else {
      state->challenger_frames = 1;
    }
    state->challenger = challenger;

    if (challenger != GestureKind::None &&
        state->challenger_frames >= config_.steal_confirm_frames) {
      // Confirmed. The old gesture ENDS on this frame and the new one begins
      // on a later one, through the same enter-threshold path as any other
      // gesture -- so a release and an unrelated press never arrive from the
      // same frame, and the one-shot Began still fires exactly once.
      ev.phase = GestureEvent::Phase::Ended;
      state->active = GestureKind::None;
      state->challenger = GestureKind::None;
      state->challenger_frames = 0;
      out.push_back(ev);
      continue;
    }

    ev.phase = GestureEvent::Phase::Held;
    out.push_back(ev);
  }

  // Tracks that were not in `hands` have left the frame. Each releases
  // whatever it was holding and is dropped -- spec defect 4, where shared
  // state kept firing for a hand that was no longer there.
  //
  // Emitted after the present hands' events so that one frame's output is
  // ordered predictably: updates first, then releases.
  for (TrackState& t : tracks_) {
    if (!t.in_use || t.seen_this_frame) continue;

    if (t.active != GestureKind::None) {
      GestureEvent ev{};
      ev.track_id = t.track_id;
      ev.handedness = t.latched_handedness;
      ev.kind = t.active;
      ev.phase = GestureEvent::Phase::Ended;
      ev.position = t.last_position;  // No current observation to read.
      ev.delta = Point2{0.0f, 0.0f};
      ev.timestamp_us = timestamp_us;
      out.push_back(ev);
    }
    t.in_use = false;
  }
}

void GestureRecognizer::InvalidateTrack(std::uint32_t track_id) {
  if (TrackState* t = Find(track_id)) t->in_use = false;
}

void GestureRecognizer::Reset() {
  for (TrackState& t : tracks_) t.in_use = false;
}

}  // namespace hc
