#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hc/frame_source.hpp"

namespace hc {

// Runs an inner IFrameSource on its own capture thread and always hands the
// consumer the NEWEST frame available, counting the frames it skipped past.
// This decouples capture rate from processing rate: a slow consumer gets
// fresh frames instead of an ever-growing backlog of stale ones.
//
// THREE buffers, not two. At any instant one buffer is being filled by the
// capture thread, one holds the newest published frame, and one may be held
// by the consumer. Two buffers would not be enough: once the consumer holds
// the front buffer, the capture thread's only remaining buffer is the one
// being read, which is both a data race and a visibly torn frame. The three
// slots are rotated under the mutex, which is also what gives the consumer a
// happens-before edge to the pixels the capture thread wrote.
//
// Dropping is the intended behavior, not a bug; dropped() reports it.
//
// Threading: Next(), Close(), dropped() and the destructor may be called
// from any one consumer thread, and Close() may additionally be called
// concurrently from another thread (that is how a blocked Next() gets woken).
// The inner source is touched only by the capture thread, except for the
// inner Close() that runs after the capture thread has been joined.
class LatestFrameSource : public IFrameSource {
 public:
  // Takes ownership of `inner` and immediately starts the capture thread.
  // `inner` must be non-null.
  //
  // The inner source's Next() must return within a bounded time: Close()
  // (and therefore the destructor) waits for an in-flight inner Next() to
  // return before joining, because interrupting it would mean touching the
  // inner source from two threads at once.
  explicit LatestFrameSource(std::unique_ptr<IFrameSource> inner);

  // Closes and joins the capture thread if the caller has not already done
  // so, so destroying a source mid-capture is always clean.
  ~LatestFrameSource() override;

  LatestFrameSource(const LatestFrameSource&) = delete;
  LatestFrameSource& operator=(const LatestFrameSource&) = delete;

  // Blocks until a frame is available, until the inner source ends or fails,
  // or until Close() is called. Returns false in the latter two cases, and
  // false forever afterwards.
  //
  // The `index` in `out` is the INNER source's index, passed through
  // unchanged, so a gap larger than one means frames were dropped.
  //
  // LIFETIME: as per IFrameSource::Next -- `out.view` points into one of
  // this source's three internal buffers and stays valid until the next
  // Next() or Close() call, or until destruction. The capture thread never
  // writes to the buffer the consumer is currently holding.
  bool Next(Frame& out) override;

  // Ends the source and joins the capture thread. Idempotent: a second call
  // neither double-joins nor throws. Wakes any consumer blocked in Next(),
  // which then returns false. After Close(), Next() returns false forever,
  // even if a captured frame was still pending.
  void Close() override;

  // Number of captured frames that were discarded because a newer frame
  // arrived before the consumer took them.
  std::uint64_t dropped() const;

 private:
  // One of the three rotating buffers plus the metadata of the frame it
  // currently holds.
  struct Slot {
    std::vector<std::uint8_t> pixels;
    Frame frame{};
  };

  void CaptureLoop();
  // Copies `in`'s pixels into slot `slot` and rewrites that slot's frame
  // metadata to point at them. Called only by the capture thread, only on
  // the slot it owns, and only outside the lock.
  void StoreInto(int slot, const Frame& in);

  std::unique_ptr<IFrameSource> inner_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::array<Slot, 3> slots_;
  // Slot indices, all distinct whenever valid. `capturing_` is the slot the
  // capture thread owns and is always a valid index. `ready_` is the newest
  // published frame, or -1 when the consumer has already taken it. `held_`
  // is the slot whose view the consumer is holding, or -1 before the first
  // successful Next().
  int capturing_ = 0;
  int ready_ = -1;
  int held_ = -1;
  std::uint64_t dropped_ = 0;
  bool inner_ended_ = false;  // Capture thread saw inner Next() return false.
  bool closed_ = false;       // Close() was called.
  bool done_ = false;         // Next() has returned false at least once.

  // Guards the join + inner close so that two threads calling Close() at the
  // same time cannot double-join. Never held by the capture thread.
  std::mutex close_mu_;
  bool joined_ = false;

  std::thread thread_;
};

}  // namespace hc
