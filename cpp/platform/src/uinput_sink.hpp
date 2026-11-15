#pragma once

// The Linux output sink, via a virtual input device in the kernel.
//
// WHY THIS AND NOT XTest. The plan's Task 9 was built on XTest, which on a
// Wayland session returns success from every call and moves nothing -- and
// X11 is not reachable on the development machine at all, since GNOME 50
// ships no Xorg session. uinput sidesteps the display protocol entirely: it
// asks the KERNEL to create a device, so the compositor sees events arrive as
// if from hardware and routes them like any mouse or keyboard. The same sink
// therefore works under Wayland and X11, and under any compositor, because
// none of them is being asked for permission to do anything unusual.
//
// VERIFIED EMPIRICALLY, not assumed: `cpp/tools/uinput_probe` creates the
// device, reads its own evdev node back to prove the kernel emitted the
// event, and then moves the cursor visibly. On 2026-11-15 it reported
// `EVENT WAS EMITTED` and the cursor moved on this Wayland session.
//
// Nothing is linked. open, ioctl and write are libc, so unlike libei this
// adds no build-time system dependency -- the project forbids those.
//
// THE ONE SETUP STEP: /dev/uinput is root-only by default. Create() returns
// nullptr with EACCES until the owner grants access, and the application
// prints the udev rule needed. That is a deliberate one-time decision by the
// machine's owner, not something a program should arrange for itself.

#include <memory>
#include <string>

#include "hc/command.hpp"
#include "hc/output_sink.hpp"

namespace hc {

#ifdef __linux__

// How many pixels a normalized delta of 1.0 should travel. Read from
// /sys/class/drm, which needs no display connection -- the point of this sink
// is that it talks to no display server. Returns false when no connected
// output can be read, and leaves the arguments untouched.
bool DetectScreenSize(int& width, int& height);

class UinputSink : public IOutputSink {
 public:
  // `screen_width` and `screen_height` are what a normalized delta of 1.0 is
  // multiplied by. This is the second and last place in the project that
  // turns normalized coordinates into pixels; nothing above IOutputSink may
  // learn a resolution.
  //
  // Returns nullptr when /dev/uinput cannot be opened or the device cannot be
  // created, and writes a human-readable reason -- INCLUDING the udev rule to
  // fix a permission failure -- to `error`. An unavailable sink that cannot
  // say why is the trap this whole design is avoiding.
  static std::unique_ptr<UinputSink> Create(int screen_width,
                                            int screen_height,
                                            std::string& error);

  // Releases everything held, destroys the virtual device and closes the
  // descriptor. The device disappearing is what guarantees no key can be left
  // down by a crash: the kernel releases a vanished device's keys itself.
  ~UinputSink() override;

  UinputSink(const UinputSink&) = delete;
  UinputSink& operator=(const UinputSink&) = delete;

  void Send(const Command& command) override;

  // Releases every button and key still held, in press order. Safe twice and
  // safe on a sink holding nothing.
  void ReleaseAll() override;

  bool Available() const override { return fd_ >= 0; }

  // The /dev/input/eventN node the kernel gave our device, or empty if it
  // could not be found. Reported at startup because "which device is moving
  // my cursor" is otherwise unanswerable, and the tests read it back.
  const std::string& DeviceNode() const { return node_; }

  // The device name the kernel advertises. Deliberately recognisable in
  // `libinput list-devices`, so a user can see what is injecting.
  static const char* DeviceName();

  // How long Create() waits after creating the device before it is safe to
  // write to it.
  //
  // THE ONE TIMING TRAP IN THIS APPROACH: the kernel creates the device
  // immediately, but udev and the compositor's input stack discover it
  // asynchronously, and events written before libinput has opened the node go
  // nowhere at all. That is a silent failure of exactly the XTest kind, which
  // is why it is paid once here rather than hoped away.
  static constexpr int kSettleMs = 400;

  // How long the destructor waits between releasing held keys and destroying
  // the device, so the releases are consumed before the device they belong to
  // disappears. Paid once, on shutdown. See the destructor.
  static constexpr int kDestroySettleMs = 30;

 private:
  UinputSink(int fd, int screen_width, int screen_height, std::string node);

  bool Emit(unsigned short type, unsigned short code, int value);
  bool Sync();

  void MoveRelative(const Point2& delta);
  void Scroll(const Point2& delta);
  void ClickOnce(MouseButton button);
  void KeyDown(Key key);
  void KeyUp(Key key);

  int fd_;
  int screen_width_;
  int screen_height_;
  std::string node_;

  // Leftover sub-pixel motion, carried to the next frame.
  //
  // NOT an optimisation -- without it the cursor does not move at all under
  // the shipped configuration. REL_X and REL_Y are integers, smoothing makes
  // most frames' deltas a fraction of a pixel, and truncating each one
  // independently throws every fraction away. XTest's absolute positioning
  // did not have this problem, which is why it is easy to port a bug into.
  float residual_x_ = 0.0f;
  float residual_y_ = 0.0f;
  float residual_wheel_ = 0.0f;

  // Press order, not a set, so ReleaseAll's output order is deterministic.
  static constexpr int kMaxHeld = 16;
  Key held_keys_[kMaxHeld]{};
  int held_key_count_ = 0;
};

#endif  // __linux__

}  // namespace hc
