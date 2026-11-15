// Empirical /dev/uinput probe, written for the same reason xtest_probe.cpp
// exists: do not build a sink on the assumption that injection works.
//
// uinput is the third injection route considered for this project, after XTest
// (silently dead under Wayland) and libei (needs a system library and a portal
// permission dance). It creates a VIRTUAL INPUT DEVICE in the kernel, so the
// compositor sees events arrive as if from real hardware -- which is why it
// works under Wayland and X11 alike, and why no display protocol is involved.
//
//   cmake --build cpp/build --target uinput_probe
//   ./cpp/build/tools/uinput_probe
//
// WHAT THIS CAN AND CANNOT PROVE. It reads the virtual device's own evdev node
// back, so "the kernel accepted the event and emitted it" is verified
// objectively. Whether the COMPOSITOR then moved the cursor cannot be read
// back on Wayland -- that is the whole reason XTest's failure was invisible --
// so the probe also performs a large, deliberate, reversed cursor movement and
// asks the person running it what they saw. An honest "look at the screen" is
// worth more than a check that cannot fail.
//
// Nothing is linked: open, ioctl and write are libc. The project forbids
// adding a build-time system dependency, and unlike libei this route needs
// none at all.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// How long to wait after UI_DEV_CREATE before sending anything.
//
// The kernel creates the device immediately, but udev and the compositor's
// input stack discover it asynchronously: events written before libinput has
// opened the node are delivered to nobody at all. This is the uinput
// equivalent of XTest's silent failure, and the one timing trap in the whole
// approach.
constexpr int kSettleMs = 400;

void SleepMs(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }

bool Emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
  struct input_event ev{};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  return write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

bool Sync(int fd) { return Emit(fd, EV_SYN, SYN_REPORT, 0); }

// Finds the /dev/input/eventN node belonging to the device we just created, by
// name. Needed because the probe reads its own events back, and because the
// sink will want to report which node it created.
std::string FindEventNode(const char* device_name) {
  DIR* dir = opendir("/dev/input");
  if (dir == nullptr) return {};

  std::string found;
  while (const struct dirent* entry = readdir(dir)) {
    if (std::strncmp(entry->d_name, "event", 5) != 0) continue;

    const std::string path = std::string("/dev/input/") + entry->d_name;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;  // Not ours to read; skip rather than complain.

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

}  // namespace

int main() {
  const char* kDeviceName = "hand-controller probe";

  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    std::printf("open(/dev/uinput) failed: %s\n", std::strerror(errno));
    if (errno == EACCES || errno == EPERM) {
      std::printf(
          "\nRESULT: NO PERMISSION. The device exists but is root-only here.\n"
          "Grant access once, then log out and back in:\n"
          "\n"
          "  echo 'KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\", "
          "OPTIONS+=\"static_node=uinput\"' \\\n"
          "    | sudo tee /etc/udev/rules.d/99-hand-controller-uinput.rules\n"
          "  sudo usermod -aG input \"$USER\"\n"
          "  sudo udevadm control --reload-rules && sudo udevadm trigger\n"
          "\n"
          "The group change only takes effect on a new login session.\n");
    } else if (errno == ENOENT) {
      std::printf(
          "\nRESULT: NO /dev/uinput. The uinput module is not available.\n"
          "Try: sudo modprobe uinput\n");
    }
    return 2;
  }
  std::printf("open(/dev/uinput): ok\n");

  // What the virtual device claims it can do. A compositor routes events by
  // these capabilities, so a device that does not advertise REL_X will have
  // its motion ignored no matter what it writes.
  const int ev_bits[] = {EV_KEY, EV_REL, EV_SYN};
  for (const int bit : ev_bits) {
    if (ioctl(fd, UI_SET_EVBIT, bit) < 0) {
      std::printf("UI_SET_EVBIT(%d) failed: %s\n", bit, std::strerror(errno));
      close(fd);
      return 2;
    }
  }
  const int rel_bits[] = {REL_X, REL_Y, REL_WHEEL};
  for (const int bit : rel_bits) ioctl(fd, UI_SET_RELBIT, bit);

  // BTN_LEFT matters beyond clicking: libinput classifies a device with
  // relative axes AND a left button as a pointer, and treats one without
  // buttons differently.
  const int key_bits[] = {BTN_LEFT,  BTN_RIGHT, BTN_MIDDLE, KEY_UP,
                          KEY_DOWN,  KEY_LEFT,  KEY_RIGHT,  KEY_SYSRQ,
                          KEY_ESC,   KEY_ENTER, KEY_SPACE,  KEY_TAB};
  for (const int bit : key_bits) ioctl(fd, UI_SET_KEYBIT, bit);

  struct uinput_setup setup{};
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1d6b;   // Linux Foundation, the conventional choice for
  setup.id.product = 0x0001;  // a virtual device. Not a real product id.
  setup.id.version = 1;
  std::snprintf(setup.name, sizeof(setup.name), "%s", kDeviceName);

  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
    std::printf("UI_DEV_SETUP failed: %s\n", std::strerror(errno));
    close(fd);
    return 2;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    std::printf("UI_DEV_CREATE failed: %s\n", std::strerror(errno));
    close(fd);
    return 2;
  }
  std::printf("UI_DEV_CREATE: ok, device named \"%s\"\n", kDeviceName);

  std::printf("waiting %d ms for udev and the compositor to pick it up\n",
              kSettleMs);
  SleepMs(kSettleMs);

  // --- objective half: did the kernel emit what we wrote? -----------------

  const std::string node = FindEventNode(kDeviceName);
  if (node.empty()) {
    std::printf(
        "could not find our own /dev/input/event* node (readable ones were "
        "searched)\n");
  } else {
    std::printf("found our evdev node: %s\n", node.c_str());
  }

  bool kernel_delivered = false;
  if (!node.empty()) {
    const int read_fd = open(node.c_str(), O_RDONLY | O_NONBLOCK);
    if (read_fd < 0) {
      std::printf("could not open %s for reading: %s\n", node.c_str(),
                  std::strerror(errno));
    } else {
      // One pixel, so the objective check does not also move the cursor
      // anywhere a person would notice.
      Emit(fd, EV_REL, REL_X, 1);
      Sync(fd);
      SleepMs(50);

      struct input_event ev{};
      while (read(read_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_REL && ev.code == REL_X && ev.value == 1) {
          kernel_delivered = true;
          break;
        }
      }
      close(read_fd);

      Emit(fd, EV_REL, REL_X, -1);  // Put it back.
      Sync(fd);
    }
  }

  std::printf("\nkernel round-trip: %s\n",
              kernel_delivered ? "EVENT WAS EMITTED (read back from evdev)"
                               : "could not confirm");

  // --- subjective half: did the compositor move the cursor? ---------------
  //
  // Unreadable on Wayland by design, so it is shown rather than asserted.

  std::printf(
      "\nnow moving the cursor 300 px right, then back, twice.\n"
      "WATCH THE SCREEN.\n");
  SleepMs(600);

  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < 30; ++i) {
      Emit(fd, EV_REL, REL_X, 10);
      Sync(fd);
      SleepMs(8);
    }
    SleepMs(250);
    for (int i = 0; i < 30; ++i) {
      Emit(fd, EV_REL, REL_X, -10);
      Sync(fd);
      SleepMs(8);
    }
    SleepMs(250);
  }

  ioctl(fd, UI_DEV_DESTROY);
  close(fd);

  std::printf(
      "\nRESULT: the kernel %s.\n"
      "If the cursor visibly slid right and back, uinput injection WORKS on\n"
      "this session and the sink can be built on it. If it did not move, the\n"
      "compositor ignored a device it accepted -- say so, because that is the\n"
      "same class of silent failure XTest has and it changes the plan.\n",
      kernel_delivered ? "accepted and emitted our events"
                       : "did not confirm the event round-trip");
  return kernel_delivered ? 0 : 1;
}
