#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "hc/output_sink.hpp"

namespace hc {

// A sink that records what it was asked to do instead of doing it.
//
// Two jobs, and they are the same job: `--sink log` lets a user see what the
// gestures would have done without the program touching their desktop, and
// the test suite uses the same recording to assert what the pipeline decided.
// A sink built only for tests would drift from the one users run.
//
// Lives in hc_control rather than platform/ for the same reason NullSink
// does: there is nothing platform-specific here, and the configurations that
// need it most are the ones that must not require OpenCV.
class LoggingSink : public IOutputSink {
 public:
  // With no stream, this records silently. With one, each command is also
  // written as a line of text -- that is what `--sink log` passes std::cout
  // to. The stream is borrowed and must outlive the sink.
  explicit LoggingSink(std::ostream* out = nullptr);

  void Send(const Command& command) override;

  // Appends a KeyRelease for every key still held, in the order they were
  // pressed, and holds nothing afterwards. Safe to call twice and safe on a
  // sink holding nothing -- both are pinned by tests, because this runs on
  // shutdown paths including the abnormal ones, where "called more than once"
  // is normal.
  void ReleaseAll() override;

  bool Available() const override { return true; }

  // Everything sent, in order, INCLUDING the releases ReleaseAll generated.
  // A test that could not see those could not tell a clean shutdown from a
  // stuck key.
  const std::vector<Command>& Commands() const { return commands_; }

  // Keys currently held down, in press order. Empty after ReleaseAll.
  const std::vector<Key>& HeldKeys() const { return held_; }

  void Clear();

  // One command as a single line, e.g. "move_cursor 0.021 -0.004",
  // "click left", "key_press arrow_right". Used for the stream output and
  // worth having separately so a test can assert on text without a stream.
  static std::string Describe(const Command& command);

 private:
  std::ostream* out_;
  std::vector<Command> commands_;

  // Press order, not a set: ReleaseAll's output order has to be deterministic
  // for a test to assert on it, and "the order they went down" is the only
  // ordering that needs no explanation.
  std::vector<Key> held_;
};

}  // namespace hc
