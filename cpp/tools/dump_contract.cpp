// Dumps every contract the UI layer needs, as JSON, GENERATED FROM THE REAL
// CODE rather than transcribed from it.
//
// This exists because a UI built against a hand-written mock encodes guesses
// about names, shapes and rules, and those guesses are only discovered to be
// wrong at integration. Running this instead makes the mock the real shape:
// the gesture and command names come from the same functions the bindings
// parser uses, the default configuration is the file that ships, and the
// sample session is produced by actually running GestureRecognizer and
// ActionMapper over synthetic poses.
//
// Re-run it whenever hc_control changes and diff the output. A field that
// moved will show up here before it shows up as a broken screen.
//
//   cmake --build cpp/build --target dump_contract
//   ./cpp/build/tools/dump_contract > contract.json
//
// The JSON is hand-written rather than produced by a library: the project
// forbids fetched dependencies, and the output shape is fixed and small.

#include <cstdio>
#include <string>
#include <vector>

#include "hc/action_mapper.hpp"
#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/gesture_recognizer.hpp"
#include "hc/hand_features.hpp"
#include "hc/hand_observation.hpp"

namespace {

// --- minimal JSON emission ------------------------------------------------

std::string Escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (const char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string Str(const std::string& s) { return "\"" + Escape(s) + "\""; }

// Six decimals: normalized coordinates live in 0..1, so this is well under a
// pixel on any display anyone owns, and it keeps the output diffable.
std::string Num(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
  return std::string(buf);
}

std::string Point(const hc::Point2& p) {
  return "[" + Num(p.x) + ", " + Num(p.y) + "]";
}

const char* HandednessName(hc::Handedness h) {
  switch (h) {
    case hc::Handedness::Left: return "left";
    case hc::Handedness::Right: return "right";
    case hc::Handedness::Unknown: return "unknown";
  }
  return "unknown";
}

const char* PhaseName(hc::GestureEvent::Phase p) {
  switch (p) {
    case hc::GestureEvent::Phase::Began: return "began";
    case hc::GestureEvent::Phase::Held: return "held";
    case hc::GestureEvent::Phase::Ended: return "ended";
  }
  return "began";
}

// What argument each command takes, so a UI can render the right control --
// a button picker, a key picker, or nothing -- without hardcoding the list.
const char* CommandArgKind(hc::Command::Kind kind) {
  switch (kind) {
    case hc::Command::Kind::Click:
    case hc::Command::Kind::DoubleClick:
      return "mouse_button_optional";
    case hc::Command::Kind::KeyPress:
    case hc::Command::Kind::KeyRelease:
      return "key_required";
    case hc::Command::Kind::MoveCursorRelative:
    case hc::Command::Kind::Scroll:
      return "none";
    case hc::Command::Kind::None:
      return "none";
  }
  return "none";
}

// --- synthetic poses, for the sample session ------------------------------
//
// Deliberately the same layout the recognizer tests use, with wide margins
// against the feature thresholds, so the sample is about shapes and ranges
// rather than about a pose sitting on a boundary.

enum Lm {
  kWrist = 0,
  kThumbCmc = 1, kThumbMcp = 2, kThumbIp = 3, kThumbTip = 4,
  kIndexMcp = 5, kIndexPip = 6, kIndexDip = 7, kIndexTip = 8,
  kMiddleMcp = 9, kMiddlePip = 10, kMiddleDip = 11, kMiddleTip = 12,
  kRingMcp = 13, kRingPip = 14, kRingDip = 15, kRingTip = 16,
  kPinkyMcp = 17, kPinkyPip = 18, kPinkyDip = 19, kPinkyTip = 20,
};

constexpr float kCol[4] = {-0.05f, -0.01f, 0.03f, 0.07f};
constexpr int kMcp[4] = {kIndexMcp, kMiddleMcp, kRingMcp, kPinkyMcp};
constexpr int kPip[4] = {kIndexPip, kMiddlePip, kRingPip, kPinkyPip};
constexpr int kDip[4] = {kIndexDip, kMiddleDip, kRingDip, kPinkyDip};
constexpr int kTip[4] = {kIndexTip, kMiddleTip, kRingTip, kPinkyTip};

hc::HandObservation OpenPalm(std::uint32_t track_id, hc::Handedness hand,
                             hc::Point2 origin) {
  hc::HandObservation o{};
  o.track_id = track_id;
  o.handedness = hand;
  o.handedness_score = 0.9f;
  o.presence = 1.0f;

  const auto put = [&](int i, float dx, float dy) {
    o.landmarks[i] = hc::Point2{origin.x + dx, origin.y + dy};
  };
  put(kWrist, 0.0f, 0.0f);
  put(kThumbCmc, -0.03f, -0.02f);
  put(kThumbMcp, -0.07f, -0.05f);
  put(kThumbIp, -0.11f, -0.07f);
  put(kThumbTip, -0.20f, -0.09f);
  for (int f = 0; f < 4; ++f) {
    put(kMcp[f], kCol[f], -0.10f);
    put(kPip[f], kCol[f], -0.18f);
    put(kDip[f], kCol[f], -0.24f);
    put(kTip[f], kCol[f], -0.30f);
  }
  return o;
}

void Curl(hc::HandObservation& o, int f) {
  const hc::Point2 pip = o.landmarks[kPip[f]];
  o.landmarks[kDip[f]] = hc::Point2{pip.x, pip.y + 0.03f};
  o.landmarks[kTip[f]] = hc::Point2{pip.x, pip.y + 0.06f};
}

void FoldThumb(hc::HandObservation& o) {
  const hc::Point2 index_mcp = o.landmarks[kIndexMcp];
  o.landmarks[kThumbIp] =
      hc::Point2{index_mcp.x - 0.01f, index_mcp.y + 0.02f};
  o.landmarks[kThumbTip] =
      hc::Point2{index_mcp.x + 0.01f, index_mcp.y + 0.01f};
}

// Index only: the pose the default configuration binds to cursor movement.
hc::HandObservation PointIndex(std::uint32_t id, hc::Handedness hand,
                               hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(id, hand, origin);
  for (int f = 1; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

hc::HandObservation Fist(std::uint32_t id, hc::Handedness hand,
                         hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(id, hand, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Thumb only: the default binds this on the left hand to the right arrow,
// pressed on `began` and released on `ended`.
hc::HandObservation ThumbUp(std::uint32_t id, hc::Handedness hand,
                            hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(id, hand, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  return o;
}

// --- emission -------------------------------------------------------------

void DumpVocabulary() {
  std::printf("  \"vocabulary\": {\n");

  std::printf("    \"gestures\": [");
  bool first = true;
  for (std::size_t i = 0; i < hc::kGestureKindCount; ++i) {
    const hc::GestureKind kind = static_cast<hc::GestureKind>(i);
    if (kind == hc::GestureKind::None) continue;  // Never bindable.
    std::printf("%s%s", first ? "" : ", ",
                Str(hc::GestureKindName(kind)).c_str());
    first = false;
  }
  std::printf("],\n");

  std::printf("    \"phases\": [\"began\", \"held\", \"ended\"],\n");
  std::printf("    \"hands\": [\"any\", \"left\", \"right\"],\n");
  std::printf(
      "    \"handedness_values\": [\"unknown\", \"left\", \"right\"],\n");

  std::printf("    \"commands\": [\n");
  const hc::Command::Kind kinds[] = {
      hc::Command::Kind::MoveCursorRelative, hc::Command::Kind::Scroll,
      hc::Command::Kind::Click,              hc::Command::Kind::DoubleClick,
      hc::Command::Kind::KeyPress,           hc::Command::Kind::KeyRelease,
  };
  for (std::size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    std::printf("      {\"name\": %s, \"argument\": %s}%s\n",
                Str(hc::CommandKindName(kinds[i])).c_str(),
                Str(CommandArgKind(kinds[i])).c_str(),
                i + 1 == sizeof(kinds) / sizeof(kinds[0]) ? "" : ",");
  }
  std::printf("    ],\n");

  std::printf("    \"mouse_buttons\": [");
  const hc::MouseButton buttons[] = {hc::MouseButton::Left,
                                     hc::MouseButton::Right,
                                     hc::MouseButton::Middle};
  for (std::size_t i = 0; i < 3; ++i) {
    std::printf("%s%s", i ? ", " : "",
                Str(hc::MouseButtonName(buttons[i])).c_str());
  }
  std::printf("],\n");

  std::printf("    \"keys\": [");
  const hc::Key keys[] = {hc::Key::ArrowUp,  hc::Key::ArrowDown,
                          hc::Key::ArrowLeft, hc::Key::ArrowRight,
                          hc::Key::PrintScreen, hc::Key::Escape,
                          hc::Key::Enter,    hc::Key::Space,
                          hc::Key::Tab};
  for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    std::printf("%s%s", i ? ", " : "", Str(hc::KeyName(keys[i])).c_str());
  }
  std::printf("]\n");

  std::printf("  },\n");
}

void DumpSettings() {
  std::printf("  \"settings\": {\n");
  std::printf(
      "    \"sensitivity\": {\"default\": %s, \"exclusive_minimum\": 0, "
      "\"note\": \"multiplies normalized motion; above 1 lets a small hand "
      "movement cross a wide display\"},\n",
      Num(hc::kDefaultSensitivity).c_str());
  std::printf(
      "    \"smoothing\": {\"default\": %s, \"minimum\": 0, \"maximum\": 1, "
      "\"note\": \"0 is no smoothing; 1 would freeze the cursor entirely\"}\n",
      Num(hc::kDefaultSmoothing).c_str());
  std::printf("  },\n");
}

// The recognizer's own tuning. NOT part of the bindings file and not settable
// from a UI today -- it is here because it explains behaviour a UI has to
// render honestly: why a gesture survives one finger of flicker, and why
// switching between two one-finger-apart poses takes several frames.
void DumpRecognizer() {
  const hc::GestureConfig c = hc::GestureConfig::Default();

  std::printf("  \"recognizer\": {\n");
  std::printf("    \"note\": %s,\n",
              Str("Read-only. Scores are in 0..1. A gesture starts at `enter` "
                  "and survives until it falls below `exit`; the gap is the "
                  "hysteresis. Pattern gestures score in fifths of the five "
                  "fingers, so exit 0.7 tolerates one finger of flicker.")
                  .c_str());
  std::printf("    \"pinch_span\": %s,\n", Num(c.pinch_span).c_str());
  std::printf("    \"steal_confirm_frames\": %d,\n", c.steal_confirm_frames);
  std::printf("    \"steal_note\": %s,\n",
              Str("Five pairs differ by one finger -- fist/point_index, "
                  "fist/thumb_up, fist/pinky_only, open_palm/four_fingers, "
                  "point_index/two_fingers. Between those, a higher-scoring "
                  "gesture must win this many CONSECUTIVE frames before it "
                  "takes over, and the takeover emits `ended` on one frame "
                  "and `began` on a later one.")
                  .c_str());
  std::printf("    \"thresholds\": {\n");
  bool first = true;
  for (std::size_t i = 0; i < hc::kGestureKindCount; ++i) {
    const hc::GestureKind kind = static_cast<hc::GestureKind>(i);
    if (kind == hc::GestureKind::None) continue;  // Slot is never read.
    std::printf("%s      %s: {\"enter\": %s, \"exit\": %s}", first ? "" : ",\n",
                Str(hc::GestureKindName(kind)).c_str(),
                Num(c.thresholds[i].enter).c_str(),
                Num(c.thresholds[i].exit).c_str());
    first = false;
  }
  std::printf("\n    }\n");
  std::printf("  },\n");
}

// The rules the parser ENFORCES. A UI that does not apply them can produce a
// file the application then refuses to load, which is a worse experience than
// not offering the choice.
void DumpRules() {
  std::printf("  \"validation_rules\": [\n");
  const char* rules[] = {
      "A binding is hand + gesture + phase -> command. The phase is part of "
      "the key, not a detail: `began` fires once, `held` fires every frame, "
      "`ended` fires on release.",
      "No two bindings may OVERLAP on the same gesture and phase. `any` "
      "overlaps `left` and `right`, so `any pinch began` plus `left pinch "
      "began` is rejected even though the lines differ.",
      "Every `key_press` must have a matching `key_release` for the same "
      "hand, gesture and key, on the `ended` phase. A file without it is "
      "REJECTED, so a UI must not let the user save one.",
      "A lone `key_release` with no `key_press` is allowed: releasing an "
      "unpressed key is harmless.",
      "`gesture` and `command` may not be \"none\", even though both appear "
      "in the enums.",
      "`move_cursor` and `scroll` take no argument. `click` and "
      "`double_click` take an optional mouse button, defaulting to left. "
      "`key_press` and `key_release` require a key.",
      "sensitivity must be > 0. smoothing must be within 0..1 inclusive. "
      "Non-finite values (nan, inf) are rejected.",
      "An EMPTY configuration is valid and means no bindings with default "
      "settings. It is not an error state.",
  };
  for (std::size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
    std::printf("    %s%s\n", Str(rules[i]).c_str(),
                i + 1 == sizeof(rules) / sizeof(rules[0]) ? "" : ",");
  }
  std::printf("  ],\n");
}

void DumpBinding(const hc::Binding& b) {
  std::printf("      {\"hand\": %s, \"gesture\": %s, \"phase\": %s, "
              "\"command\": %s",
              Str(hc::HandSelectorName(b.hand)).c_str(),
              Str(hc::GestureKindName(b.gesture)).c_str(),
              Str(PhaseName(b.phase)).c_str(),
              Str(hc::CommandKindName(b.command.kind)).c_str());
  switch (b.command.kind) {
    case hc::Command::Kind::Click:
    case hc::Command::Kind::DoubleClick:
      std::printf(", \"button\": %s",
                  Str(hc::MouseButtonName(b.command.button)).c_str());
      break;
    case hc::Command::Kind::KeyPress:
    case hc::Command::Kind::KeyRelease:
      std::printf(", \"key\": %s", Str(hc::KeyName(b.command.key)).c_str());
      break;
    default:
      break;
  }
  std::printf("}");
}

bool DumpDefaultConfiguration(hc::BindingSet& out) {
  std::string error;
  if (!hc::ParseBindings(hc::DefaultBindingsText(), out, error)) {
    std::fprintf(stderr, "the shipped default does not parse: %s\n",
                 error.c_str());
    return false;
  }

  std::printf("  \"default_configuration\": {\n");
  std::printf("    \"sensitivity\": %s,\n", Num(out.sensitivity).c_str());
  std::printf("    \"smoothing\": %s,\n", Num(out.smoothing).c_str());
  std::printf("    \"bindings\": [\n");
  for (std::size_t i = 0; i < out.bindings.size(); ++i) {
    DumpBinding(out.bindings[i]);
    std::printf("%s\n", i + 1 == out.bindings.size() ? "" : ",");
  }
  std::printf("    ],\n");

  // The file verbatim, so a UI can show it and round-trip it through
  // ParseBindings/SerializeBindings without reconstructing the format.
  std::printf("    \"text\": %s\n", Str(hc::DefaultBindingsText()).c_str());
  std::printf("  },\n");
  return true;
}

void DumpObservation(const hc::HandObservation& o) {
  const hc::HandFeatures f = hc::ComputeFeatures(o);
  std::printf("          {\"track_id\": %u, \"handedness\": %s, "
              "\"handedness_score\": %s, \"presence\": %s,\n",
              o.track_id, Str(HandednessName(o.handedness)).c_str(),
              Num(o.handedness_score).c_str(), Num(o.presence).c_str());
  std::printf("           \"landmarks\": [");
  for (std::size_t i = 0; i < o.landmarks.size(); ++i) {
    std::printf("%s%s", i ? ", " : "", Point(o.landmarks[i]).c_str());
  }
  std::printf("],\n");
  std::printf("           \"features\": {\"extended\": [%s, %s, %s, %s, %s], "
              "\"pinch_distance\": %s, \"palm_center\": %s, "
              "\"palm_size\": %s}}",
              f.extended[0] ? "true" : "false",
              f.extended[1] ? "true" : "false",
              f.extended[2] ? "true" : "false",
              f.extended[3] ? "true" : "false",
              f.extended[4] ? "true" : "false",
              Num(f.pinch_distance).c_str(), Point(f.palm_center).c_str(),
              Num(f.palm_size).c_str());
}

void DumpCommand(const hc::Command& c) {
  std::printf("          {\"kind\": %s",
              Str(hc::CommandKindName(c.kind)).c_str());
  switch (c.kind) {
    case hc::Command::Kind::MoveCursorRelative:
    case hc::Command::Kind::Scroll:
      std::printf(", \"delta\": %s", Point(c.delta).c_str());
      break;
    case hc::Command::Kind::Click:
    case hc::Command::Kind::DoubleClick:
      std::printf(", \"button\": %s",
                  Str(hc::MouseButtonName(c.button)).c_str());
      break;
    case hc::Command::Kind::KeyPress:
    case hc::Command::Kind::KeyRelease:
      std::printf(", \"key\": %s", Str(hc::KeyName(c.key)).c_str());
      break;
    case hc::Command::Kind::None:
      break;
  }
  std::printf("}");
}

// A real session, produced by running the actual recognizer and mapper. This
// is what Live View renders and what Diagnostics counts, so a UI mocking it
// from this output is mocking the real thing.
//
// The script: a right hand points and drifts rightward for 11 frames (cursor
// movement), a left hand holds a thumb-up for the middle of that (an arrow
// key pressed and released), and the right hand closes to a fist at the end
// (bound to nothing, so it demonstrates the unbound case).
//
// That last transition is also the one a UI is most likely to get wrong:
// point_index and fist differ by a single finger, so the recognizer does not
// switch on the frame the pose changes. It holds for steal_confirm_frames,
// then emits `ended` on one frame and `began` on the NEXT. A Live View that
// assumes the two arrive together will drop a frame of state.
void DumpSampleSession(const hc::BindingSet& config) {
  hc::GestureRecognizer recognizer(hc::GestureConfig::Default());
  hc::ActionMapper mapper(config);

  std::printf("  \"sample_session\": {\n");
  std::printf("    \"note\": %s,\n",
              Str("Produced by running GestureRecognizer and ActionMapper "
                  "over synthetic poses. Landmarks are frame-normalized "
                  "0..1, 21 per hand, MediaPipe order. Deltas are relative "
                  "and normalized -- never pixels.")
                  .c_str());
  std::printf("    \"frames\": [\n");

  std::vector<hc::GestureEvent> events;
  std::vector<hc::Command> commands;

  // Long enough that the fist at the end gets past its takeover confirmation
  // and is seen HELD, not only begun on the final frame.
  const int kFrames = 16;
  for (int frame = 0; frame < kFrames; ++frame) {
    const std::int64_t ts = static_cast<std::int64_t>(frame) * 11000;
    std::vector<hc::HandObservation> hands;

    // Right hand: points and drifts right, then closes into a fist.
    const float drift = 0.45f + static_cast<float>(frame) * 0.012f;
    if (frame < 11) {
      hands.push_back(
          PointIndex(1, hc::Handedness::Right, {drift, 0.80f}));
    } else {
      hands.push_back(Fist(1, hc::Handedness::Right, {drift, 0.80f}));
    }

    // Left hand: thumb-up for frames 4..9, absent otherwise, so the sample
    // contains a hand appearing and leaving as well as a press and release.
    if (frame >= 4 && frame <= 9) {
      hands.push_back(ThumbUp(2, hc::Handedness::Left, {0.20f, 0.80f}));
    }

    for (hc::HandObservation& h : hands) h.timestamp_us = ts;

    recognizer.Process(hands, ts, events);
    mapper.Map(events, commands);

    std::printf("      {\n        \"frame\": %d,\n        "
                "\"timestamp_us\": %lld,\n",
                frame, static_cast<long long>(ts));

    std::printf("        \"hands\": [\n");
    for (std::size_t i = 0; i < hands.size(); ++i) {
      DumpObservation(hands[i]);
      std::printf("%s\n", i + 1 == hands.size() ? "" : ",");
    }
    std::printf("        ],\n");

    std::printf("        \"events\": [\n");
    for (std::size_t i = 0; i < events.size(); ++i) {
      const hc::GestureEvent& e = events[i];
      std::printf("          {\"track_id\": %u, \"handedness\": %s, "
                  "\"gesture\": %s, \"phase\": %s, \"position\": %s, "
                  "\"delta\": %s}%s\n",
                  e.track_id, Str(HandednessName(e.handedness)).c_str(),
                  Str(hc::GestureKindName(e.kind)).c_str(),
                  Str(PhaseName(e.phase)).c_str(), Point(e.position).c_str(),
                  Point(e.delta).c_str(), i + 1 == events.size() ? "" : ",");
    }
    std::printf("        ],\n");

    std::printf("        \"commands\": [\n");
    for (std::size_t i = 0; i < commands.size(); ++i) {
      DumpCommand(commands[i]);
      std::printf("%s\n", i + 1 == commands.size() ? "" : ",");
    }
    std::printf("        ]\n");
    std::printf("      }%s\n", frame + 1 == kFrames ? "" : ",");
  }

  std::printf("    ]\n");
  std::printf("  }\n");
}

}  // namespace

int main() {
  std::printf("{\n");
  std::printf("  \"schema_version\": 1,\n");
  std::printf("  \"generated_by\": \"cpp/tools/dump_contract.cpp\",\n");
  std::printf("  \"note\": %s,\n",
              Str("Generated from hc_control. Do not hand-edit: re-run the "
                  "tool and diff. See docs/superpowers/specs/"
                  "2026-11-15-gui-contract.md for the prose version.")
                  .c_str());

  DumpVocabulary();
  DumpSettings();
  DumpRecognizer();
  DumpRules();

  hc::BindingSet config;
  if (!DumpDefaultConfiguration(config)) return 1;

  DumpSampleSession(config);

  std::printf("}\n");
  return 0;
}
