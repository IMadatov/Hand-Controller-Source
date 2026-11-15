#include "sendinput_sink.hpp"

#ifdef _WIN32

// Never compiled. See the header: there is no Windows machine here, so
// everything below is unexercised by even a syntax check.

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace hc {
namespace {

// A normalized delta of 1.0 means "the whole screen", so this is the only
// scaling there is. Pixels end here and never travel back up.
int ToPixels(float normalized, int extent) {
  if (!std::isfinite(normalized)) return 0;
  return static_cast<int>(std::lround(static_cast<double>(normalized) *
                                      static_cast<double>(extent)));
}

// Windows' absolute mouse coordinates are a 0..65535 range over the primary
// display, not pixels.
LONG ToAbsolute(int pixel, int extent) {
  if (extent <= 1) return 0;
  const double fraction = static_cast<double>(pixel) /
                          static_cast<double>(extent - 1);
  const double clamped = std::min(1.0, std::max(0.0, fraction));
  return static_cast<LONG>(std::lround(clamped * 65535.0));
}

// Virtual-key codes, and whether the key is an "extended" one. The arrows and
// print-screen are extended; omitting the flag makes them arrive as their
// numeric-keypad twins on some layouts, which is the classic SendInput bug.
struct VkCode {
  WORD vk;
  bool extended;
};

VkCode ToVk(Key key) {
  switch (key) {
    case Key::ArrowUp: return {VK_UP, true};
    case Key::ArrowDown: return {VK_DOWN, true};
    case Key::ArrowLeft: return {VK_LEFT, true};
    case Key::ArrowRight: return {VK_RIGHT, true};
    case Key::PrintScreen: return {VK_SNAPSHOT, true};
    case Key::Escape: return {VK_ESCAPE, false};
    case Key::Enter: return {VK_RETURN, false};
    case Key::Space: return {VK_SPACE, false};
    case Key::Tab: return {VK_TAB, false};
  }
  return {VK_ESCAPE, false};
}

struct ButtonFlags {
  DWORD down;
  DWORD up;
};

ButtonFlags ToButtonFlags(MouseButton button) {
  switch (button) {
    case MouseButton::Left:
      return {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP};
    case MouseButton::Right:
      return {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP};
    case MouseButton::Middle:
      return {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP};
  }
  return {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP};
}

std::size_t ButtonIndex(MouseButton button) {
  switch (button) {
    case MouseButton::Left: return 0;
    case MouseButton::Right: return 1;
    case MouseButton::Middle: return 2;
  }
  return 0;
}

void SendOne(INPUT& input) {
  // Return value deliberately ignored: SendInput fails when a
  // higher-integrity process holds the foreground window (UIPI), and there is
  // nothing useful to do about it per event. Available() answers the question
  // the application actually asks, and a per-call failure is not a reason to
  // stop the frame loop.
  SendInput(1, &input, sizeof(INPUT));
}

}  // namespace

std::unique_ptr<SendInputSink> SendInputSink::Create() {
  const int width = GetSystemMetrics(SM_CXSCREEN);
  const int height = GetSystemMetrics(SM_CYSCREEN);

  // Zero means there is no desktop to measure -- a service session. A sink
  // that cannot scale a delta cannot move a cursor, so say so here rather
  // than dividing by it later.
  if (width <= 1 || height <= 1) return nullptr;

  return std::unique_ptr<SendInputSink>(new SendInputSink(width, height));
}

SendInputSink::SendInputSink(int screen_width, int screen_height)
    : screen_width_(screen_width),
      screen_height_(screen_height),
      available_(true) {}

SendInputSink::~SendInputSink() {
  // The last line of defence for review focus 7: a key still down outlives
  // the process, and this runs on paths the code above forgot.
  ReleaseAll();
}

void SendInputSink::MoveRelative(const Point2& delta) {
  // Absolute positioning over MOUSEEVENTF_MOVE, deliberately: relative moves
  // are run through the user's pointer speed and acceleration curve, so the
  // same hand movement would cover a different distance on two machines and
  // `sensitivity` would stop meaning anything. Read, add, clamp, write.
  POINT current{};
  if (!GetCursorPos(&current)) return;

  const int target_x = static_cast<int>(current.x) +
                       ToPixels(delta.x, screen_width_);
  const int target_y = static_cast<int>(current.y) +
                       ToPixels(delta.y, screen_height_);

  // CLAMPED, NOT WRAPPED. A cursor that reappears on the far edge when a hand
  // drifts past the screen is disorienting; one that stops at the edge is
  // what every pointing device does.
  const int clamped_x = std::min(screen_width_ - 1, std::max(0, target_x));
  const int clamped_y = std::min(screen_height_ - 1, std::max(0, target_y));

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = ToAbsolute(clamped_x, screen_width_);
  input.mi.dy = ToAbsolute(clamped_y, screen_height_);
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  SendOne(input);
}

void SendInputSink::Scroll(const Point2& delta) {
  // Only y is meaningful, per Command. The sign is inverted because screen y
  // grows downward while a positive wheel delta scrolls the content up:
  // moving the hand down should move the page down.
  const double notches =
      -static_cast<double>(delta.y) * static_cast<double>(screen_height_) /
      static_cast<double>(WHEEL_DELTA);
  const int amount = static_cast<int>(std::lround(notches * WHEEL_DELTA));
  if (amount == 0) return;  // Sub-notch motion; sending 0 is a no-op anyway.

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.mouseData = static_cast<DWORD>(amount);
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  SendOne(input);
}

void SendInputSink::ClickOnce(MouseButton button) {
  const ButtonFlags flags = ToButtonFlags(button);

  INPUT down{};
  down.type = INPUT_MOUSE;
  down.mi.dwFlags = flags.down;
  SendOne(down);

  INPUT up{};
  up.type = INPUT_MOUSE;
  up.mi.dwFlags = flags.up;
  SendOne(up);

  // Not recorded as held: the pair is sent together, so there is nothing for
  // ReleaseAll to clean up. held_buttons_ exists for a future binding that
  // holds a button down across frames, which the vocabulary does not express
  // yet.
  held_buttons_[ButtonIndex(button)] = false;
}

void SendInputSink::KeyDown(Key key) {
  const VkCode code = ToVk(key);

  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = code.vk;
  input.ki.dwFlags = code.extended ? KEYEVENTF_EXTENDEDKEY : 0;
  SendOne(input);

  // Recorded once: a repeated press of an already-held key must not queue two
  // releases, or ReleaseAll would send a second release for a key already up.
  for (int i = 0; i < held_key_count_; ++i) {
    if (held_keys_[i] == key) return;
  }
  if (held_key_count_ < kMaxHeld) held_keys_[held_key_count_++] = key;
}

void SendInputSink::KeyUp(Key key) {
  const VkCode code = ToVk(key);

  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = code.vk;
  input.ki.dwFlags = KEYEVENTF_KEYUP |
                     (code.extended ? KEYEVENTF_EXTENDEDKEY : 0);
  SendOne(input);

  for (int i = 0; i < held_key_count_; ++i) {
    if (held_keys_[i] != key) continue;
    for (int j = i; j + 1 < held_key_count_; ++j) {
      held_keys_[j] = held_keys_[j + 1];
    }
    --held_key_count_;
    return;
  }
}

void SendInputSink::Send(const Command& command) {
  switch (command.kind) {
    case Command::Kind::MoveCursorRelative:
      MoveRelative(command.delta);
      break;
    case Command::Kind::Scroll:
      Scroll(command.delta);
      break;
    case Command::Kind::Click:
      ClickOnce(command.button);
      break;
    case Command::Kind::DoubleClick:
      // Two clicks back to back. WHETHER THE TARGET APPLICATION SEES A DOUBLE
      // CLICK IS ITS OWN DECISION, made from the interval against
      // GetDoubleClickTime; this sink cannot guarantee it and does not
      // pretend to. Sending them with no delay is the best it can do.
      ClickOnce(command.button);
      ClickOnce(command.button);
      break;
    case Command::Kind::KeyPress:
      KeyDown(command.key);
      break;
    case Command::Kind::KeyRelease:
      KeyUp(command.key);
      break;
    case Command::Kind::None:
      break;  // Inert by design.
  }
}

void SendInputSink::ReleaseAll() {
  // Copied out first: KeyUp mutates held_keys_ as it goes.
  Key keys[kMaxHeld]{};
  const int count = held_key_count_;
  for (int i = 0; i < count; ++i) keys[i] = held_keys_[i];
  for (int i = 0; i < count; ++i) KeyUp(keys[i]);

  for (std::size_t i = 0; i < 3; ++i) {
    if (!held_buttons_[i]) continue;
    const MouseButton button = static_cast<MouseButton>(i);
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = ToButtonFlags(button).up;
    SendOne(up);
    held_buttons_[i] = false;
  }

  // Idempotent by construction: both loops are now over empty state, so a
  // second call sends nothing. Shutdown paths call this more than once.
}

}  // namespace hc

#endif  // _WIN32
