#include "uinput_sink.hpp"

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace hc {
namespace {

// The capabilities the device advertises. A compositor routes events by these
// alone: a device that does not claim REL_X has its motion dropped no matter
// what it writes, and libinput only classifies something as a POINTER if it
// has relative axes together with a left button.
const int kRelAxes[] = {REL_X, REL_Y, REL_WHEEL};

const int kKeys[] = {
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE,
    KEY_UP,   KEY_DOWN,  KEY_LEFT,  KEY_RIGHT, KEY_SYSRQ,
    KEY_ESC,  KEY_ENTER, KEY_SPACE, KEY_TAB,
};

// hc::Key is abstract by design (see command.hpp: a Command carrying a keysym
// or a VK code would make the logic layer unportable). This is the Linux half
// of that bargain, and the only place in the project that knows a KEY_ code.
int ToKeyCode(Key key) {
  switch (key) {
    case Key::ArrowUp: return KEY_UP;
    case Key::ArrowDown: return KEY_DOWN;
    case Key::ArrowLeft: return KEY_LEFT;
    case Key::ArrowRight: return KEY_RIGHT;
    case Key::PrintScreen: return KEY_SYSRQ;  // "SysRq" is print-screen.
    case Key::Escape: return KEY_ESC;
    case Key::Enter: return KEY_ENTER;
    case Key::Space: return KEY_SPACE;
    case Key::Tab: return KEY_TAB;
  }
  return KEY_ESC;
}

int ToButtonCode(MouseButton button) {
  switch (button) {
    case MouseButton::Left: return BTN_LEFT;
    case MouseButton::Right: return BTN_RIGHT;
    case MouseButton::Middle: return BTN_MIDDLE;
  }
  return BTN_LEFT;
}

void SleepMs(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }

// Splits a float into whole units to send now and a fraction to carry.
// std::trunc rather than floor so that motion either side of zero is treated
// symmetrically -- flooring would bias every leftward movement by a pixel.
int TakeWhole(float& residual, float amount) {
  residual += amount;
  if (!std::isfinite(residual)) {
    // A non-finite delta would poison the accumulator for the rest of the
    // session, so it is dropped rather than carried.
    residual = 0.0f;
    return 0;
  }
  const float whole = std::trunc(residual);
  residual -= whole;
  return static_cast<int>(whole);
}

// Finds the /dev/input/eventN node belonging to a device with this name.
//
// Returns empty when none is readable, which is NOT an error: the sink works
// regardless, and this is only used for reporting and for the tests. A user
// with no read access to /dev/input gets a working sink and no node name.
std::string FindEventNode(const char* device_name) {
  DIR* dir = opendir("/dev/input");
  if (dir == nullptr) return {};

  std::string found;
  while (const struct dirent* entry = readdir(dir)) {
    if (std::strncmp(entry->d_name, "event", 5) != 0) continue;

    const std::string path = std::string("/dev/input/") + entry->d_name;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
        std::strcmp(name, device_name) == 0) {
      found = path;
      close(fd);
      break;
    }
    close(fd);
  }
  closedir(dir);
  return found;
}

const char* kUdevAdvice =
    "/dev/uinput is root-only by default. Grant access once:\n"
    "  echo 'KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\", "
    "OPTIONS+=\"static_node=uinput\"' \\\n"
    "    | sudo tee /etc/udev/rules.d/99-hand-controller-uinput.rules\n"
    "  sudo usermod -aG input \"$USER\"\n"
    "  sudo udevadm control --reload-rules && sudo udevadm trigger\n"
    "The group change takes effect in a new login session.";

}  // namespace

// Reads the first mode of the first connected output. /sys/class/drm needs no
// display server, which is the point: this sink deliberately talks to none,
// and asking Wayland for a screen size would mean a protocol dependency it
// exists to avoid.
bool DetectScreenSize(int& width, int& height) {
  DIR* dir = opendir("/sys/class/drm");
  if (dir == nullptr) return false;

  bool found = false;
  while (const struct dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.find('-') == std::string::npos) continue;  // Not a connector.

    std::ifstream status("/sys/class/drm/" + name + "/status");
    std::string state;
    if (!status || !(status >> state) || state != "connected") continue;

    // First line is the active/preferred mode, as "1920x1080".
    std::ifstream modes("/sys/class/drm/" + name + "/modes");
    std::string mode;
    if (!modes || !(modes >> mode)) continue;

    const std::size_t x = mode.find('x');
    if (x == std::string::npos) continue;

    const int w = std::atoi(mode.substr(0, x).c_str());
    const int h = std::atoi(mode.substr(x + 1).c_str());
    if (w <= 0 || h <= 0) continue;

    width = w;
    height = h;
    found = true;
    break;
  }
  closedir(dir);
  return found;
}

const char* UinputSink::DeviceName() { return "hand-controller virtual input"; }

std::unique_ptr<UinputSink> UinputSink::Create(int screen_width,
                                               int screen_height,
                                               std::string& error) {
  error.clear();

  if (screen_width <= 0 || screen_height <= 0) {
    error = "screen size must be positive, got " +
            std::to_string(screen_width) + "x" + std::to_string(screen_height);
    return nullptr;
  }

  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    error = std::string("could not open /dev/uinput: ") + std::strerror(errno);
    if (errno == EACCES || errno == EPERM) {
      error += "\n";
      error += kUdevAdvice;
    } else if (errno == ENOENT) {
      error += "\nThe uinput module may not be loaded: sudo modprobe uinput";
    }
    return nullptr;
  }

  const int ev_bits[] = {EV_KEY, EV_REL, EV_SYN};
  for (const int bit : ev_bits) {
    if (ioctl(fd, UI_SET_EVBIT, bit) >= 0) continue;
    error = std::string("UI_SET_EVBIT failed: ") + std::strerror(errno);
    close(fd);
    return nullptr;
  }
  for (const int bit : kRelAxes) ioctl(fd, UI_SET_RELBIT, bit);
  for (const int bit : kKeys) ioctl(fd, UI_SET_KEYBIT, bit);

  struct uinput_setup setup{};
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1d6b;   // Linux Foundation; the conventional id for a
  setup.id.product = 0x0001;  // virtual device, not a real product.
  setup.id.version = 1;
  std::snprintf(setup.name, sizeof(setup.name), "%s", DeviceName());

  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
    error = std::string("UI_DEV_SETUP failed: ") + std::strerror(errno);
    close(fd);
    return nullptr;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    error = std::string("UI_DEV_CREATE failed: ") + std::strerror(errno);
    close(fd);
    return nullptr;
  }

  // Paid once per session, never per event. See kSettleMs: writing before
  // libinput has opened the node delivers to nobody.
  SleepMs(kSettleMs);

  return std::unique_ptr<UinputSink>(new UinputSink(
      fd, screen_width, screen_height, FindEventNode(DeviceName())));
}

UinputSink::UinputSink(int fd, int screen_width, int screen_height,
                       std::string node)
    : fd_(fd),
      screen_width_(screen_width),
      screen_height_(screen_height),
      node_(std::move(node)) {}

UinputSink::~UinputSink() {
  if (fd_ < 0) return;

  ReleaseAll();

  // A beat between the releases and the device vanishing.
  //
  // Writing a key-up and destroying the device microseconds later is a race
  // against whoever is reading: the events are queued on a device that is
  // about to stop existing. The kernel's input core does synthesise releases
  // for a removed device, so a key would probably not stick either way --
  // but "probably" is the word that made XTest's failure invisible, and this
  // costs 30 ms once, on shutdown. It is also what makes the destructor's
  // release observable to a test at all.
  SleepMs(kDestroySettleMs);

  ioctl(fd_, UI_DEV_DESTROY);
  close(fd_);
  fd_ = -1;
}

bool UinputSink::Emit(unsigned short type, unsigned short code, int value) {
  if (fd_ < 0) return false;

  struct input_event ev{};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  // Return value checked but not reported per event: a write failing here
  // means the device went away, and there is nothing useful to do about it
  // thirty times a second. The application asks Available() instead.
  return write(fd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

bool UinputSink::Sync() { return Emit(EV_SYN, SYN_REPORT, 0); }

void UinputSink::MoveRelative(const Point2& delta) {
  // NOT CLAMPED, and deliberately: relative motion is what a mouse produces,
  // so the compositor stops the cursor at the screen edge itself. XTest
  // needed a clamp because it positioned absolutely and would have wrapped.
  const int dx = TakeWhole(residual_x_,
                           delta.x * static_cast<float>(screen_width_));
  const int dy = TakeWhole(residual_y_,
                           delta.y * static_cast<float>(screen_height_));
  if (dx == 0 && dy == 0) return;  // Sub-pixel; carried, not lost.

  if (dx != 0) Emit(EV_REL, REL_X, dx);
  if (dy != 0) Emit(EV_REL, REL_Y, dy);
  Sync();
}

void UinputSink::Scroll(const Point2& delta) {
  // Only y is meaningful, per Command. REL_WHEEL counts NOTCHES, not pixels,
  // so a full screen of hand movement is deliberately a modest number of
  // them: scrolling a page per twitch would be unusable.
  constexpr float kNotchesPerScreen = 15.0f;
  const int notches =
      TakeWhole(residual_wheel_, -delta.y * kNotchesPerScreen);
  if (notches == 0) return;

  // Negated above because screen y grows downward while a positive wheel
  // value scrolls content up: moving the hand down should move the page down.
  Emit(EV_REL, REL_WHEEL, notches);
  Sync();
}

void UinputSink::ClickOnce(MouseButton button) {
  const int code = ToButtonCode(button);
  Emit(EV_KEY, static_cast<unsigned short>(code), 1);
  Sync();
  Emit(EV_KEY, static_cast<unsigned short>(code), 0);
  Sync();

  // Not recorded as held: the pair is emitted together, so there is nothing
  // for ReleaseAll to clean up. A binding that holds a button across frames
  // is not expressible in the vocabulary yet.
}

void UinputSink::KeyDown(Key key) {
  Emit(EV_KEY, static_cast<unsigned short>(ToKeyCode(key)), 1);
  Sync();

  // Recorded once. A repeated press of an already-held key must not queue two
  // releases, or ReleaseAll would emit a key-up for a key already up -- which
  // a compositor is entitled to find confusing.
  for (int i = 0; i < held_key_count_; ++i) {
    if (held_keys_[i] == key) return;
  }
  if (held_key_count_ < kMaxHeld) held_keys_[held_key_count_++] = key;
}

void UinputSink::KeyUp(Key key) {
  Emit(EV_KEY, static_cast<unsigned short>(ToKeyCode(key)), 0);
  Sync();

  for (int i = 0; i < held_key_count_; ++i) {
    if (held_keys_[i] != key) continue;
    for (int j = i; j + 1 < held_key_count_; ++j) {
      held_keys_[j] = held_keys_[j + 1];
    }
    --held_key_count_;
    return;
  }
}

void UinputSink::Send(const Command& command) {
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
      // Two clicks in a row. WHETHER THE APPLICATION SEES A DOUBLE CLICK IS
      // ITS OWN DECISION, made from the interval between them against its
      // double-click time. This sink cannot guarantee it and does not
      // pretend to.
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

void UinputSink::ReleaseAll() {
  // Copied out first: KeyUp mutates held_keys_ as it goes.
  Key keys[kMaxHeld]{};
  const int count = held_key_count_;
  for (int i = 0; i < count; ++i) keys[i] = held_keys_[i];
  for (int i = 0; i < count; ++i) KeyUp(keys[i]);

  // Buttons are never left down (see ClickOnce), but the releases are emitted
  // anyway: this runs on crash paths, and a button this process believes is
  // up while the compositor believes it is down is exactly the state worth
  // spending three events to rule out.
  for (const MouseButton button :
       {MouseButton::Left, MouseButton::Right, MouseButton::Middle}) {
    Emit(EV_KEY, static_cast<unsigned short>(ToButtonCode(button)), 0);
  }
  Sync();

  // Idempotent: the loop above is now over empty state and the button
  // releases are no-ops on an already-released button.
}

}  // namespace hc

#endif  // __linux__
