#pragma once

// The Windows output sink, via SendInput.
//
// WRITTEN BLIND AND NEVER RUN. There is no Windows machine on this project,
// so this file has never been compiled, let alone executed: on Linux the
// guard below removes it entirely, which means not even the compiler has
// checked it. Phase 1's README already states that Windows is unverified and
// this does not change that claim -- it is a starting point for the first
// person with a Windows box, not a supported backend.
//
// There is deliberately NO TEST for it. A test that cannot run on the only
// machine available would assert nothing while looking like coverage, which
// is worse than the honest gap.

#include <memory>

#include "hc/command.hpp"
#include "hc/output_sink.hpp"

namespace hc {

#ifdef _WIN32

// Injects input through the Win32 SendInput API.
//
// This is the second and last place in the project permitted to turn
// normalized coordinates into pixels (the overlay is the first). Nothing
// above IOutputSink may learn a screen resolution.
class SendInputSink : public IOutputSink {
 public:
  // Returns nullptr when the metrics needed to scale a normalized delta into
  // pixels are unavailable -- a session with no desktop, which is what a
  // service account gets. Reporting that is the point: a sink that cannot
  // work must say so rather than appearing to run.
  static std::unique_ptr<SendInputSink> Create();

  ~SendInputSink() override;

  void Send(const Command& command) override;

  // Releases every button and key still held, in press order.
  void ReleaseAll() override;

  bool Available() const override { return available_; }

 private:
  SendInputSink(int screen_width, int screen_height);

  void MoveRelative(const Point2& delta);
  void Scroll(const Point2& delta);
  void ClickOnce(MouseButton button);
  void KeyDown(Key key);
  void KeyUp(Key key);

  int screen_width_;
  int screen_height_;
  bool available_;

  // Press order, not a set, so ReleaseAll's output order is deterministic.
  // Fixed arrays rather than vectors: Send runs on the frame loop, and the
  // vocabulary is small and closed.
  static constexpr int kMaxHeld = 16;
  Key held_keys_[kMaxHeld]{};
  int held_key_count_ = 0;
  bool held_buttons_[3]{};
};

#endif  // _WIN32

}  // namespace hc
