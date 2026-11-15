#include "hc/command.hpp"

#include <cstddef>
#include <cstring>

namespace hc {
namespace {

// A name table per enum, so that the spelling used when writing a
// configuration and the spelling accepted when reading one cannot drift
// apart -- both directions walk the same array.
template <typename T>
struct NameEntry {
  const char* name;
  T value;
};

constexpr NameEntry<Command::Kind> kCommandKinds[] = {
    {"none", Command::Kind::None},
    {"move_cursor", Command::Kind::MoveCursorRelative},
    {"click", Command::Kind::Click},
    {"double_click", Command::Kind::DoubleClick},
    {"scroll", Command::Kind::Scroll},
    {"key_press", Command::Kind::KeyPress},
    {"key_release", Command::Kind::KeyRelease},
};

constexpr NameEntry<MouseButton> kMouseButtons[] = {
    {"left", MouseButton::Left},
    {"right", MouseButton::Right},
    {"middle", MouseButton::Middle},
};

constexpr NameEntry<Key> kKeys[] = {
    {"arrow_up", Key::ArrowUp},
    {"arrow_down", Key::ArrowDown},
    {"arrow_left", Key::ArrowLeft},
    {"arrow_right", Key::ArrowRight},
    {"print_screen", Key::PrintScreen},
    {"escape", Key::Escape},
    {"enter", Key::Enter},
    {"space", Key::Space},
    {"tab", Key::Tab},
};

template <typename T, std::size_t N>
const char* LookupName(const NameEntry<T> (&table)[N], T value,
                       const char* fallback) {
  for (const auto& entry : table) {
    if (entry.value == value) return entry.name;
  }
  return fallback;
}

template <typename T, std::size_t N>
bool LookupValue(const NameEntry<T> (&table)[N], const char* name, T& out) {
  if (name == nullptr) return false;
  for (const auto& entry : table) {
    if (std::strcmp(name, entry.name) == 0) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

}  // namespace

const char* CommandKindName(Command::Kind kind) {
  return LookupName(kCommandKinds, kind, "none");
}

bool CommandKindFromName(const char* name, Command::Kind& out) {
  return LookupValue(kCommandKinds, name, out);
}

const char* MouseButtonName(MouseButton button) {
  return LookupName(kMouseButtons, button, "left");
}

bool MouseButtonFromName(const char* name, MouseButton& out) {
  return LookupValue(kMouseButtons, name, out);
}

const char* KeyName(Key key) {
  return LookupName(kKeys, key, "escape");
}

bool KeyFromName(const char* name, Key& out) {
  return LookupValue(kKeys, name, out);
}

}  // namespace hc
