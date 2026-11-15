#include "hc/bindings.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "hc/default_bindings.hpp"

namespace hc {
namespace {

// Phase names, indexed by GestureEvent::Phase.
const char* PhaseName(GestureEvent::Phase phase) {
  switch (phase) {
    case GestureEvent::Phase::Began: return "began";
    case GestureEvent::Phase::Held: return "held";
    case GestureEvent::Phase::Ended: return "ended";
  }
  return "began";
}

bool PhaseFromName(const std::string& name, GestureEvent::Phase& out) {
  if (name == "began") { out = GestureEvent::Phase::Began; return true; }
  if (name == "held") { out = GestureEvent::Phase::Held; return true; }
  if (name == "ended") { out = GestureEvent::Phase::Ended; return true; }
  return false;
}

std::string LinePrefix(std::size_t line) {
  return "line " + std::to_string(line) + ": ";
}

// Parses a complete string as a float. Stricter than atof deliberately:
// trailing garbage ("0.5x"), an empty string, and the non-finite spellings
// ("nan", "inf") are all errors rather than silently accepted values. A
// smoothing of NaN would pass a naive 0..1 range check, since every
// comparison against NaN is false.
bool ParseFloat(const std::string& text, float& out) {
  if (text.empty()) return false;

  errno = 0;
  char* end = nullptr;
  const float v = std::strtof(text.c_str(), &end);
  if (end != text.c_str() + text.size()) return false;
  if (errno == ERANGE) return false;
  if (!(v == v)) return false;                      // NaN
  if (v > 3.4e38f || v < -3.4e38f) return false;    // Inf
  out = v;
  return true;
}

// Formats a float with enough significant digits that ParseFloat returns the
// identical value. A float needs 9; "%g" alone would emit 6 and quietly lose
// the round trip SerializeBindings promises.
std::string FormatFloat(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return std::string(buf);
}

std::vector<std::string> Tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream in(line);
  std::string token;
  while (in >> token) tokens.push_back(token);
  return tokens;
}

// Two hand selectors overlap when a single hand could satisfy both. `Any`
// overlaps everything, including itself.
bool HandsOverlap(HandSelector a, HandSelector b) {
  if (a == HandSelector::Any || b == HandSelector::Any) return true;
  return a == b;
}

}  // namespace

const char* HandSelectorName(HandSelector hand) {
  switch (hand) {
    case HandSelector::Any: return "any";
    case HandSelector::Left: return "left";
    case HandSelector::Right: return "right";
  }
  return "any";
}

bool HandSelectorFromName(const char* name, HandSelector& out) {
  if (name == nullptr) return false;
  if (std::strcmp(name, "any") == 0) { out = HandSelector::Any; return true; }
  if (std::strcmp(name, "left") == 0) { out = HandSelector::Left; return true; }
  if (std::strcmp(name, "right") == 0) { out = HandSelector::Right; return true; }
  return false;
}

bool HandSelectorMatches(HandSelector selector, Handedness handedness) {
  switch (selector) {
    case HandSelector::Any:
      // Including Handedness::Unknown. That is the entire reason Any exists:
      // phase 1 measured the label's confidence as low as 0.575, so a hand
      // the model could not name must still be bindable.
      return true;
    case HandSelector::Left:
      return handedness == Handedness::Left;
    case HandSelector::Right:
      return handedness == Handedness::Right;
  }
  return false;
}

bool ParseBindings(const std::string& text, BindingSet& out,
                   std::string& error) {
  out = BindingSet{};
  error.clear();

  // Line numbers of each accepted binding, so a later cross-binding check can
  // report where the offending line actually was rather than just naming the
  // rule.
  std::vector<std::size_t> binding_lines;

  std::istringstream in(text);
  std::string raw;
  std::size_t line_no = 0;

  while (std::getline(in, raw)) {
    ++line_no;

    const std::size_t hash = raw.find('#');
    const std::string stripped = (hash == std::string::npos)
                                     ? raw
                                     : raw.substr(0, hash);

    const std::vector<std::string> t = Tokenize(stripped);
    if (t.empty()) continue;  // Blank, whitespace-only, or comment-only.

    // --- set <name> <value> ---
    if (t[0] == "set") {
      if (t.size() != 3) {
        error = LinePrefix(line_no) +
                "'set' takes a name and a value, e.g. 'set sensitivity 2.0'";
        return false;
      }
      float value = 0.0f;
      if (!ParseFloat(t[2], value)) {
        error = LinePrefix(line_no) + "'" + t[2] + "' is not a number";
        return false;
      }
      if (t[1] == "sensitivity") {
        if (!(value > 0.0f)) {
          error = LinePrefix(line_no) +
                  "sensitivity must be greater than 0, got " + t[2];
          return false;
        }
        out.sensitivity = value;
      } else if (t[1] == "smoothing") {
        if (!(value >= 0.0f && value <= 1.0f)) {
          error = LinePrefix(line_no) +
                  "smoothing must be between 0 and 1, got " + t[2];
          return false;
        }
        out.smoothing = value;
      } else {
        error = LinePrefix(line_no) + "unknown setting '" + t[1] +
                "' (expected 'sensitivity' or 'smoothing')";
        return false;
      }
      continue;
    }

    // --- <hand> <gesture> <phase> -> <command> [arg] ---
    if (t.size() < 5 || t[3] != "->") {
      error = LinePrefix(line_no) +
              "expected '<hand> <gesture> <phase> -> <command> [arg]'";
      return false;
    }
    if (t.size() > 6) {
      error = LinePrefix(line_no) + "too many arguments after '" + t[4] + "'";
      return false;
    }

    Binding b{};

    if (!HandSelectorFromName(t[0].c_str(), b.hand)) {
      error = LinePrefix(line_no) + "unknown hand '" + t[0] +
              "' (expected 'any', 'left' or 'right')";
      return false;
    }
    if (!GestureKindFromName(t[1].c_str(), b.gesture) ||
        b.gesture == GestureKind::None) {
      error = LinePrefix(line_no) + "unknown gesture '" + t[1] + "'";
      return false;
    }
    if (!PhaseFromName(t[2], b.phase)) {
      error = LinePrefix(line_no) + "unknown phase '" + t[2] +
              "' (expected 'began', 'held' or 'ended')";
      return false;
    }
    if (!CommandKindFromName(t[4].c_str(), b.command.kind) ||
        b.command.kind == Command::Kind::None) {
      error = LinePrefix(line_no) + "unknown command '" + t[4] + "'";
      return false;
    }

    const bool has_arg = (t.size() == 6);

    switch (b.command.kind) {
      case Command::Kind::MoveCursorRelative:
      case Command::Kind::Scroll:
        if (has_arg) {
          error = LinePrefix(line_no) + "'" + t[4] +
                  "' takes no argument (it acts on the gesture's own "
                  "movement), got '" + t[5] + "'";
          return false;
        }
        break;

      case Command::Kind::Click:
      case Command::Kind::DoubleClick:
        if (has_arg && !MouseButtonFromName(t[5].c_str(), b.command.button)) {
          error = LinePrefix(line_no) + "unknown mouse button '" + t[5] +
                  "' (expected 'left', 'right' or 'middle')";
          return false;
        }
        break;

      case Command::Kind::KeyPress:
      case Command::Kind::KeyRelease:
        if (!has_arg) {
          error = LinePrefix(line_no) + "'" + t[4] + "' needs a key name";
          return false;
        }
        if (!KeyFromName(t[5].c_str(), b.command.key)) {
          error = LinePrefix(line_no) + "unknown key '" + t[5] + "'";
          return false;
        }
        break;

      case Command::Kind::None:
        break;  // Already rejected above.
    }

    // Overlap, not just exact duplication. `any` against a `left` on the same
    // gesture and phase would fire two commands from one gesture, and letting
    // the later line win would make a typo invisible.
    for (std::size_t i = 0; i < out.bindings.size(); ++i) {
      const Binding& prev = out.bindings[i];
      if (prev.gesture != b.gesture || prev.phase != b.phase) continue;
      if (!HandsOverlap(prev.hand, b.hand)) continue;

      error = LinePrefix(line_no) + "binding for '" +
              GestureKindName(b.gesture) + " " + PhaseName(b.phase) +
              "' on hand '" + HandSelectorName(b.hand) +
              "' overlaps the one on line " +
              std::to_string(binding_lines[i]) + " for hand '" +
              HandSelectorName(prev.hand) + "'";
      return false;
    }

    out.bindings.push_back(b);
    binding_lines.push_back(line_no);
  }

  // Every key_press needs its key_release. A key left down outlives the
  // gesture and the process, which is the worst failure this layer can cause,
  // so it is rejected at load rather than discovered by the user.
  for (std::size_t i = 0; i < out.bindings.size(); ++i) {
    const Binding& press = out.bindings[i];
    if (press.command.kind != Command::Kind::KeyPress) continue;

    bool paired = false;
    for (const Binding& other : out.bindings) {
      if (other.command.kind != Command::Kind::KeyRelease) continue;
      if (other.phase != GestureEvent::Phase::Ended) continue;
      if (other.hand != press.hand) continue;
      if (other.gesture != press.gesture) continue;
      if (other.command.key != press.command.key) continue;
      paired = true;
      break;
    }

    if (!paired) {
      error = LinePrefix(binding_lines[i]) + "key_press " +
              KeyName(press.command.key) + " has no matching release; add '" +
              HandSelectorName(press.hand) + " " +
              GestureKindName(press.gesture) + " ended -> key_release " +
              KeyName(press.command.key) + "' or the key stays down";
      return false;
    }
  }

  return true;
}

std::string SerializeBindings(const BindingSet& set) {
  std::ostringstream out;

  out << "set sensitivity " << FormatFloat(set.sensitivity) << "\n";
  out << "set smoothing " << FormatFloat(set.smoothing) << "\n";

  for (const Binding& b : set.bindings) {
    out << HandSelectorName(b.hand) << " " << GestureKindName(b.gesture) << " "
        << PhaseName(b.phase) << " -> " << CommandKindName(b.command.kind);

    switch (b.command.kind) {
      case Command::Kind::Click:
      case Command::Kind::DoubleClick:
        // Always written, even when it is the default, so that a round trip
        // through this function is textually stable as well as semantically
        // equal -- a serialize/parse/serialize cycle yields the same bytes.
        out << " " << MouseButtonName(b.command.button);
        break;
      case Command::Kind::KeyPress:
      case Command::Kind::KeyRelease:
        out << " " << KeyName(b.command.key);
        break;
      case Command::Kind::MoveCursorRelative:
      case Command::Kind::Scroll:
      case Command::Kind::None:
        break;
    }
    out << "\n";
  }

  return out.str();
}

const char* DefaultBindingsText() { return internal::kDefaultBindingsText; }

}  // namespace hc
