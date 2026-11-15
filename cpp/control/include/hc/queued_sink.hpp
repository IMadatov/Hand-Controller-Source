#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "hc/output_sink.hpp"

namespace hc {

// Runs another sink on its own thread so that Send never stalls the caller.
//
// Spec defect 7, and the one the spec is most emphatic about: the Python
// version paid pyautogui's 100 ms PAUSE inside the frame loop, so every click
// cost three frames of tracking. Injection can block -- an X server can stall
// for reasons that have nothing to do with this program -- and a frame loop
// that waits on it drops hands instead.
//
// A FULL QUEUE DROPS AND COUNTS. It does not wait. Preserving a backlog of
// stale cursor motion is worse than losing it, because by the time a stalled
// queue drains, every delta in it describes where the hand used to be.
//
// EXCEPT FOR RELEASES, WHICH ARE NEVER DROPPED. This is the subtle part. A
// dropped cursor delta is a cursor that moved slightly less; a dropped
// KeyRelease is a key held down after the hand is gone and after the process
// exits. The two are not comparable faults, so they are not handled the same
// way: a release entering a full queue evicts the oldest DROPPABLE item
// instead of being dropped itself.
//
// Lives in hc_control, not platform/: it is a decorator over IOutputSink
// built from std::thread and a mutex, with nothing platform-specific in it.
class QueuedSink : public IOutputSink {
 public:
  // `capacity` is the number of droppable commands held before eviction
  // starts, and must be at least 1.
  QueuedSink(std::unique_ptr<IOutputSink> inner, std::size_t capacity);

  // Releases everything the inner sink holds, then stops the worker.
  //
  // Review focus 7: every press must have a matching release on EVERY exit
  // path, and a destructor is the one path that runs even when the code above
  // forgot. Bounded by kShutdownTimeout -- see Drain.
  ~QueuedSink() override;

  QueuedSink(const QueuedSink&) = delete;
  QueuedSink& operator=(const QueuedSink&) = delete;

  // Enqueues and returns. NEVER BLOCKS on the inner sink, and never waits for
  // space: it either finds room, evicts something droppable, or drops this
  // command and counts it.
  void Send(const Command& command) override;

  // Drains the queue and releases everything the inner sink holds, THEN
  // RETURNS -- this one does block, deliberately.
  //
  // A non-blocking "please release eventually" is not a guarantee, and this
  // is the call that stands between a crash and a stuck key. It is bounded by
  // kShutdownTimeout so a wedged inner sink cannot hang the process forever;
  // if that bound is hit the keys may still be down, which is the lesser of
  // the two bad outcomes and is why the bound is generous.
  void ReleaseAll() override;

  // Sampled from the inner sink ONCE, at construction, and cached.
  //
  // The inner sink is touched only by the worker thread, so asking it
  // anything from the caller's thread would be a data race. Availability is a
  // startup property in practice -- "is there an X server to talk to" -- and
  // a sink that becomes unavailable mid-run is deliberately not modelled.
  bool Available() const override { return inner_available_; }

  // Commands dropped because the queue was full. For the shutdown report: a
  // run that dropped thousands is a run whose sink could not keep up, and
  // that should be visible rather than inferred from a sluggish cursor.
  std::size_t DroppedCount() const;

  // Blocks until the queue is empty and the worker is idle, or `timeout`
  // elapses. Returns true if it drained. Exposed because a test cannot
  // otherwise know when an asynchronous sink has caught up.
  bool Drain(std::chrono::milliseconds timeout);

  // The bound on every blocking wait in this class. Generous on purpose: it
  // exists to stop a wedged sink hanging the process, not to time anything.
  static constexpr std::chrono::milliseconds kShutdownTimeout{2000};

 private:
  // A release-all is an operation, not a Command, so it rides the same queue
  // as a flag rather than being squeezed into a Command::Kind that sinks
  // would then have to ignore.
  struct Item {
    Command command;
    bool release_all = false;
  };

  static bool IsUndroppable(const Item& item);

  void Worker();

  std::unique_ptr<IOutputSink> inner_;
  const std::size_t capacity_;
  bool inner_available_;

  mutable std::mutex mu_;
  std::condition_variable work_cv_;     // Worker waits for items.
  std::condition_variable idle_cv_;     // Drain waits for quiet.
  std::deque<Item> queue_;
  bool stopping_ = false;
  bool in_flight_ = false;              // Worker is inside an inner call.
  std::size_t dropped_ = 0;

  std::thread worker_;
};

}  // namespace hc
