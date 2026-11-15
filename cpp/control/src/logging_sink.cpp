#include "hc/logging_sink.hpp"

#include <algorithm>
#include <cstdio>
#include <ostream>

namespace hc {
namespace {

std::string Coord(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
  return std::string(buf);
}

}  // namespace

LoggingSink::LoggingSink(std::ostream* out) : out_(out) {}

std::string LoggingSink::Describe(const Command& command) {
  std::string line = CommandKindName(command.kind);
  switch (command.kind) {
    case Command::Kind::MoveCursorRelative:
    case Command::Kind::Scroll:
      line += " " + Coord(command.delta.x) + " " + Coord(command.delta.y);
      break;
    case Command::Kind::Click:
    case Command::Kind::DoubleClick:
      line += " ";
      line += MouseButtonName(command.button);
      break;
    case Command::Kind::KeyPress:
    case Command::Kind::KeyRelease:
      line += " ";
      line += KeyName(command.key);
      break;
    case Command::Kind::None:
      break;
  }
  return line;
}

void LoggingSink::Send(const Command& command) {
  commands_.push_back(command);

  // Track what is down, so ReleaseAll can put it back up. Clicks are not
  // tracked: Command has no button-press/button-release pair, so a click is
  // atomic and leaves nothing held. (If dragging is ever wanted, that is new
  // command kinds, not extra state here.)
  if (command.kind == Command::Kind::KeyPress) {
    // A repeated press of an already-held key is not an error -- a binding on
    // the `held` phase does exactly that, every frame. Recording it twice
    // would make ReleaseAll emit two releases for one key.
    if (std::find(held_.begin(), held_.end(), command.key) == held_.end()) {
      held_.push_back(command.key);
    }
  } else if (command.kind == Command::Kind::KeyRelease) {
    const auto it = std::find(held_.begin(), held_.end(), command.key);
    if (it != held_.end()) held_.erase(it);
  }

  if (out_ != nullptr) *out_ << Describe(command) << "\n";
}

void LoggingSink::ReleaseAll() {
  // Copied, because Send mutates held_ as it processes each release.
  const std::vector<Key> to_release = held_;
  for (const Key key : to_release) {
    Command c{};
    c.kind = Command::Kind::KeyRelease;
    c.key = key;
    Send(c);
  }
  held_.clear();  // Send already emptied it; this makes the guarantee local.
}

void LoggingSink::Clear() {
  commands_.clear();
  held_.clear();
}

}  // namespace hc
