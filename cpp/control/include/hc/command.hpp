#pragma once

#include "hc/geometry.hpp"

namespace hc {

// NOTHING IN THIS FILE NAMES A PLATFORM API, and that is its entire purpose.
//
// A Command is what the mapper decided should happen, expressed so that it
// means the same thing on X11, on Windows, on macOS, and to a browser client
// that cannot inject input at all and will render the command as data. Each
// OutputSink translates; the mapper never does.
//
// A Command carrying an X11 KeySym or a Windows VK_ code would make the whole
// logic layer unportable, and it is the specific mistake this type exists to
// prevent.

// Which mouse button a Click or DoubleClick refers to.
enum class MouseButton { Left, Right, Middle };

// An abstract key, deliberately NOT a keysym, NOT a virtual-key code and NOT
// a scancode. Each sink owns a translation table.
//
// The list is short on purpose. Every entry added here has to be translated
// in EVERY sink, so a generous enum is a promise that each new platform then
// has to keep. These are the keys the default configuration needs (parity
// with main.py: four arrows plus print-screen) and four that are hard to
// regret.
enum class Key {
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
  PrintScreen,
  Escape,
  Enter,
  Space,
  Tab,
};

// One abstract action.
//
// Which fields carry meaning depends on `kind`, and the rest are left at
// their defaults rather than being set to something plausible:
//
//   None                 nothing. A default-constructed Command is inert, so
//                        a sink receiving one by mistake does nothing instead
//                        of clicking. The bindings parser rejects it.
//   MoveCursorRelative   `delta` -- frame-normalized, relative. The sink
//                        scales it to pixels; see below.
//   Scroll               `delta` -- usually only y is meaningful.
//   Click, DoubleClick   `button`.
//   KeyPress, KeyRelease `key`.
//
// `delta` IS RELATIVE AND NORMALIZED, never an absolute screen position.
// Phase 1 established that the core speaks 0..1 coordinates and that exactly
// one place multiplies by pixels; OutputSink is the second and last such
// place. Relative motion is also what lets a small hand movement cross a wide
// display, which the Python version's absolute 1:1 mapping could not do.
struct Command {
  enum class Kind {
    None = 0,
    MoveCursorRelative,
    Click,
    DoubleClick,
    Scroll,
    KeyPress,
    KeyRelease,
  };

  Kind kind = Kind::None;
  Point2 delta{0.0f, 0.0f};
  MouseButton button = MouseButton::Left;
  Key key = Key::Escape;
};

// Stable lowercase names, as they appear in the bindings file: "none",
// "move_cursor", "click", "double_click", "scroll", "key_press",
// "key_release".
//
// Published interface. Do not reword once configurations exist.
const char* CommandKindName(Command::Kind kind);
bool CommandKindFromName(const char* name, Command::Kind& out);

// "left", "right", "middle".
const char* MouseButtonName(MouseButton button);
bool MouseButtonFromName(const char* name, MouseButton& out);

// "arrow_up", "arrow_down", "arrow_left", "arrow_right", "print_screen",
// "escape", "enter", "space", "tab".
const char* KeyName(Key key);
bool KeyFromName(const char* name, Key& out);

}  // namespace hc
