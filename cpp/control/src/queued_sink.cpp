#include "hc/queued_sink.hpp"

#include <cassert>
#include <utility>

namespace hc {

QueuedSink::QueuedSink(std::unique_ptr<IOutputSink> inner,
                       std::size_t capacity)
    : inner_(std::move(inner)), capacity_(capacity) {
  assert(inner_ != nullptr && "QueuedSink: inner sink must not be null");
  assert(capacity >= 1 && "QueuedSink: capacity must be at least 1");

  // Sampled before the worker exists, so this is the one moment the inner
  // sink can be touched from this thread without racing it.
  inner_available_ = inner_->Available();

  worker_ = std::thread(&QueuedSink::Worker, this);
}

QueuedSink::~QueuedSink() {
  // Release first, while the worker is still alive to do it. Doing this after
  // the join would work too, but it would mean the destructor touching the
  // inner sink directly, and keeping every inner call on one thread is what
  // makes the whole class easy to reason about.
  ReleaseAll();

  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  work_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool QueuedSink::IsUndroppable(const Item& item) {
  // A release must arrive or a key stays down. A release-all is the shutdown
  // guarantee itself. Everything else -- motion, scrolls, clicks -- is a
  // moment that has passed.
  return item.release_all ||
         item.command.kind == Command::Kind::KeyRelease;
}

void QueuedSink::Send(const Command& command) {
  Item item;
  item.command = command;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return;

    if (queue_.size() >= capacity_) {
      // Make room by evicting the oldest DROPPABLE entry -- never simply the
      // oldest. An unconditional pop_front() is the obvious way to write
      // this and it is wrong: once a release is sitting in the queue, the
      // next burst of cursor motion evicts it, and the key stays down. That
      // was a real defect here, found by the concurrent-senders test, which
      // lost exactly one release out of four.
      //
      // The scan is over at most `capacity_` entries and only runs when the
      // queue is already full, which is the path that is allowed to be slow.
      auto victim = queue_.end();
      for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (!IsUndroppable(*it)) {
          victim = it;
          break;
        }
      }

      if (victim != queue_.end()) {
        // Keep the newest: for relative motion, the newest delta describes
        // where the hand is now.
        //
        // NOTE: because motion is relative, an evicted delta is displacement
        // the cursor never travels, so sustained overload UNDER-MOVES the
        // cursor rather than lagging it. Coalescing consecutive motion
        // commands by summing their deltas would preserve the total exactly;
        // it is not done here because overload is not expected -- an XTest
        // call is microseconds, and the 100 ms figure that motivated this
        // class was pyautogui's artificial PAUSE -- and DroppedCount() is
        // what would reveal the assumption to be wrong.
        queue_.erase(victim);
        ++dropped_;
      } else if (IsUndroppable(item)) {
        // Nothing droppable to evict, and this must not be lost either, so
        // the queue is allowed over capacity. Bounded in practice: the
        // outstanding undroppable items are releases, and only as many keys
        // can be held at once as the Key enum has entries.
      } else {
        // The queue is entirely undroppable and this command is not. Drop it
        // rather than growing without bound.
        ++dropped_;
        return;
      }
    }

    queue_.push_back(item);
  }

  work_cv_.notify_one();
}

void QueuedSink::ReleaseAll() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return;
    Item item;
    item.release_all = true;
    queue_.push_back(item);  // Undroppable, and never evicted.
  }
  work_cv_.notify_one();

  // Blocking here is the point. See the header.
  Drain(kShutdownTimeout);
}

bool QueuedSink::Drain(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  return idle_cv_.wait_for(lock, timeout, [this] {
    return queue_.empty() && !in_flight_;
  });
}

std::size_t QueuedSink::DroppedCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return dropped_;
}

void QueuedSink::Worker() {
  for (;;) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(mu_);
      work_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

      if (queue_.empty()) {
        // Only leave once there is nothing left: a stop must still drain, or
        // the release sitting in the queue would never happen.
        if (stopping_) return;
        continue;
      }

      item = queue_.front();
      queue_.pop_front();
      in_flight_ = true;
    }

    // Outside the lock, because this is the call that can block. Holding the
    // mutex here would make Send wait on the very thing this class exists to
    // avoid waiting on.
    if (item.release_all) {
      inner_->ReleaseAll();
    } else {
      inner_->Send(item.command);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      in_flight_ = false;
    }
    idle_cv_.notify_all();
  }
}

}  // namespace hc
