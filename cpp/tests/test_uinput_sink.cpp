#include "uinput_sink.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hc/command.hpp"

using hc::Command;
using hc::Key;
using hc::MouseButton;
using hc::Point2;
using hc::UinputSink;

namespace {

// Every test here reads the virtual device's OWN evdev node back, which is
// what makes them real tests rather than assertions that a write returned.
// This is the half of injection that can be verified objectively; whether the
// compositor then moves the cursor cannot be read back on Wayland, and
// `cpp/tools/uinput_probe` is where a person confirms that by eye.
//
// They SKIP when /dev/uinput is not accessible, so the suite still passes on
// a machine that has not granted the one-time udev access. To run them for
// real:
//
//   sudo -u "$USER" -g input ./cpp/build-app/bin/test_uinput_sink
//
// A skipped test asserts nothing. If this file reports SKIPPED, the sink is
// UNVERIFIED on that machine -- which is the honest state, not a pass.

constexpr int kScreenW = 1920;
constexpr int kScreenH = 1080;

// Drains every pending event from the node, with a short settle so the kernel
// has delivered what was just written.
std::vector<struct input_event> Drain(int fd) {
  usleep(60 * 1000);

  std::vector<struct input_event> events;
  struct input_event ev{};
  while (read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
    events.push_back(ev);
  }
  return events;
}

int SumRel(const std::vector<struct input_event>& events, unsigned short code) {
  int total = 0;
  for (const struct input_event& ev : events) {
    if (ev.type == EV_REL && ev.code == code) total += ev.value;
  }
  return total;
}

int CountKey(const std::vector<struct input_event>& events,
             unsigned short code, int value) {
  int n = 0;
  for (const struct input_event& ev : events) {
    if (ev.type == EV_KEY && ev.code == code && ev.value == value) ++n;
  }
  return n;
}

// A sink plus an open read handle on its own node. Both are needed together
// in every test, and the node has to be opened BEFORE the events of interest
// are written -- evdev delivers nothing retroactively.
class Device {
 public:
  bool Open(std::string& reason) {
    std::string error;
    sink_ = UinputSink::Create(kScreenW, kScreenH, error);
    if (!sink_) {
      reason = error;
      return false;
    }
    if (sink_->DeviceNode().empty()) {
      reason = "the device was created but its /dev/input node is not readable";
      return false;
    }
    fd_ = open(sink_->DeviceNode().c_str(), O_RDONLY | O_NONBLOCK);
    if (fd_ < 0) {
      reason = "could not open " + sink_->DeviceNode() + " for reading";
      return false;
    }
    Drain(fd_);  // Discard anything from device creation itself.
    return true;
  }

  ~Device() {
    if (fd_ >= 0) close(fd_);
  }

  UinputSink& sink() { return *sink_; }
  std::unique_ptr<UinputSink>& owner() { return sink_; }
  int fd() const { return fd_; }

 private:
  std::unique_ptr<UinputSink> sink_;
  int fd_ = -1;
};

Command Move(float dx, float dy) {
  Command c{};
  c.kind = Command::Kind::MoveCursorRelative;
  c.delta = Point2{dx, dy};
  return c;
}

}  // namespace

#define REQUIRE_DEVICE(dev)                            \
  std::string reason;                                  \
  if (!(dev).Open(reason)) GTEST_SKIP() << reason;

TEST(UinputSink, DetectsAScreenSizeFromSysfs) {
  int width = 0;
  int height = 0;
  if (!hc::DetectScreenSize(width, height)) {
    GTEST_SKIP() << "no connected DRM output readable here";
  }
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
}

TEST(UinputSink, RejectsANonPositiveScreenSize) {
  std::string error;
  EXPECT_EQ(UinputSink::Create(0, 1080, error), nullptr);
  EXPECT_FALSE(error.empty());
}

// The permission failure is the one a user will actually hit, so the message
// has to carry the fix rather than just the errno.
TEST(UinputSink, PermissionFailureExplainsHowToFixIt) {
  std::string error;
  auto sink = UinputSink::Create(kScreenW, kScreenH, error);
  if (sink) {
    EXPECT_TRUE(sink->Available());
    EXPECT_TRUE(error.empty());
    return;  // Access granted on this machine; nothing to assert about the
             // failure path.
  }
  EXPECT_NE(error.find("/dev/uinput"), std::string::npos) << error;
  EXPECT_NE(error.find("udev"), std::string::npos)
      << "a permission error that does not say how to fix it is half an "
         "error: " << error;
}

TEST(UinputSink, MoveEmitsRelativeMotionScaledToTheScreen) {
  Device dev;
  REQUIRE_DEVICE(dev);

  // A tenth of the screen, which at 1920 is 192 px.
  dev.sink().Send(Move(0.1f, -0.05f));
  const std::vector<struct input_event> events = Drain(dev.fd());

  EXPECT_EQ(SumRel(events, REL_X), 192);
  EXPECT_EQ(SumRel(events, REL_Y), -54);
}

// THE BUG THIS SINK IS MOST LIKELY TO HAVE HAD. REL_X is an integer and
// smoothing makes most frames' deltas a fraction of a pixel, so truncating
// each frame independently throws every fraction away and the cursor never
// moves at all. XTest positioned absolutely and did not have this problem,
// which is exactly why it is easy to port a bug into.
TEST(UinputSink, SubPixelMotionAccumulatesInsteadOfVanishing) {
  Device dev;
  REQUIRE_DEVICE(dev);

  // 0.0002 of 1920 is 0.384 px -- each frame alone truncates to zero.
  for (int i = 0; i < 20; ++i) dev.sink().Send(Move(0.0002f, 0.0f));
  const std::vector<struct input_event> events = Drain(dev.fd());

  // 20 * 0.384 = 7.68 px, so 7 travel and the remainder is carried.
  EXPECT_EQ(SumRel(events, REL_X), 7);
}

TEST(UinputSink, MotionBothWaysCancelsRatherThanDrifting) {
  Device dev;
  REQUIRE_DEVICE(dev);

  // Symmetric truncation matters here: flooring instead of truncating would
  // bias every leftward frame by a pixel and the cursor would crawl left.
  for (int i = 0; i < 10; ++i) {
    dev.sink().Send(Move(0.0003f, 0.0f));
    dev.sink().Send(Move(-0.0003f, 0.0f));
  }
  const std::vector<struct input_event> events = Drain(dev.fd());

  EXPECT_EQ(SumRel(events, REL_X), 0);
}

TEST(UinputSink, NonFiniteMotionIsDroppedAndDoesNotPoisonTheAccumulator) {
  Device dev;
  REQUIRE_DEVICE(dev);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  dev.sink().Send(Move(nan, nan));
  Drain(dev.fd());

  // The accumulator must still work afterwards: a single bad frame of
  // landmarks cannot be allowed to disable the cursor for the session.
  dev.sink().Send(Move(0.1f, 0.0f));
  EXPECT_EQ(SumRel(Drain(dev.fd()), REL_X), 192);
}

TEST(UinputSink, ClickEmitsADownThenAnUp) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command c{};
  c.kind = Command::Kind::Click;
  c.button = MouseButton::Right;
  dev.sink().Send(c);

  const std::vector<struct input_event> events = Drain(dev.fd());
  EXPECT_EQ(CountKey(events, BTN_RIGHT, 1), 1);
  EXPECT_EQ(CountKey(events, BTN_RIGHT, 0), 1);
  EXPECT_EQ(CountKey(events, BTN_LEFT, 1), 0);
}

TEST(UinputSink, DoubleClickEmitsTwoCompletePairs) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command c{};
  c.kind = Command::Kind::DoubleClick;
  c.button = MouseButton::Left;
  dev.sink().Send(c);

  const std::vector<struct input_event> events = Drain(dev.fd());
  EXPECT_EQ(CountKey(events, BTN_LEFT, 1), 2);
  EXPECT_EQ(CountKey(events, BTN_LEFT, 0), 2);
}

TEST(UinputSink, ScrollEmitsWheelNotches) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command c{};
  c.kind = Command::Kind::Scroll;
  c.delta = Point2{0.0f, 0.2f};  // Hand moving down.
  dev.sink().Send(c);

  const std::vector<struct input_event> events = Drain(dev.fd());
  // Negative: screen y grows downward, a positive wheel value scrolls content
  // up, so a hand moving down must scroll down.
  EXPECT_LT(SumRel(events, REL_WHEEL), 0);
}

TEST(UinputSink, KeyPressAndReleaseEmitTheMatchingKeyCode) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command press{};
  press.kind = Command::Kind::KeyPress;
  press.key = Key::ArrowRight;
  dev.sink().Send(press);

  Command release = press;
  release.kind = Command::Kind::KeyRelease;
  dev.sink().Send(release);

  const std::vector<struct input_event> events = Drain(dev.fd());
  EXPECT_EQ(CountKey(events, KEY_RIGHT, 1), 1);
  EXPECT_EQ(CountKey(events, KEY_RIGHT, 0), 1);
}

// Review focus 7, at the one layer that can actually be checked: a key still
// down outlives the process.
TEST(UinputSink, ReleaseAllReleasesAHeldKey) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command press{};
  press.kind = Command::Kind::KeyPress;
  press.key = Key::PrintScreen;
  dev.sink().Send(press);
  ASSERT_EQ(CountKey(Drain(dev.fd()), KEY_SYSRQ, 1), 1);

  dev.sink().ReleaseAll();
  EXPECT_EQ(CountKey(Drain(dev.fd()), KEY_SYSRQ, 0), 1);
}

TEST(UinputSink, ASecondReleaseAllDoesNotReleaseTheKeyAgain) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command press{};
  press.kind = Command::Kind::KeyPress;
  press.key = Key::Space;
  dev.sink().Send(press);
  dev.sink().ReleaseAll();
  Drain(dev.fd());

  dev.sink().ReleaseAll();
  EXPECT_EQ(CountKey(Drain(dev.fd()), KEY_SPACE, 0), 0);
}

// Pressing the same key twice must not queue two releases, or a compositor
// receives a key-up for a key it already believes is up.
TEST(UinputSink, ARepeatedPressIsHeldOnce) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command press{};
  press.kind = Command::Kind::KeyPress;
  press.key = Key::ArrowUp;
  dev.sink().Send(press);
  dev.sink().Send(press);
  Drain(dev.fd());

  dev.sink().ReleaseAll();
  EXPECT_EQ(CountKey(Drain(dev.fd()), KEY_UP, 0), 1);
}

// The path that runs when the code above forgot, or when an exception is
// unwinding.
//
// Read from a CONCURRENT reader rather than after the fact, and that detail is
// the finding: destroying the device revokes every reader's fd and discards
// whatever it had buffered, so a release written microseconds before
// UI_DEV_DESTROY reaches nobody. Draining afterwards reports nothing even
// though the write happened -- which looks exactly like a stuck key and is
// not one.
//
// So this test proves what actually matters: a reader that is listening at the
// time (which is what libinput and the compositor are) DOES receive the
// release before the device goes away. It passes because the destructor waits
// kDestroySettleMs between the two; with that wait removed it fails.
TEST(UinputSink, DestructionReleasesAHeldKeyToAListeningReader) {
  Device dev;
  REQUIRE_DEVICE(dev);

  Command press{};
  press.kind = Command::Kind::KeyPress;
  press.key = Key::Escape;
  dev.sink().Send(press);
  ASSERT_EQ(CountKey(Drain(dev.fd()), KEY_ESC, 1), 1);

  std::atomic<bool> stop{false};
  std::atomic<int> releases{0};
  std::thread reader([&] {
    const int fd = dev.fd();
    while (!stop.load()) {
      struct input_event ev{};
      const ssize_t n = read(fd, &ev, sizeof(ev));
      if (n == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_KEY && ev.code == KEY_ESC && ev.value == 0) {
          releases.fetch_add(1);
        }
        continue;
      }
      usleep(2 * 1000);  // Non-blocking fd; nothing pending yet.
    }
  });

  dev.owner().reset();  // Destroys the sink while the reader is listening.

  usleep(80 * 1000);
  stop.store(true);
  reader.join();

  EXPECT_EQ(releases.load(), 1);
}

TEST(UinputSink, AnInertCommandEmitsNothing) {
  Device dev;
  REQUIRE_DEVICE(dev);

  dev.sink().Send(Command{});  // Kind::None.
  EXPECT_TRUE(Drain(dev.fd()).empty());
}
