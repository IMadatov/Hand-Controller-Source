#include "hc/queued_sink.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hc/command.hpp"
#include "hc/output_sink.hpp"

using hc::Command;
using hc::IOutputSink;
using hc::Key;
using hc::Point2;
using hc::QueuedSink;
using Clock = std::chrono::steady_clock;

namespace {

// Records what reached it, under its own lock since the worker thread is the
// one calling. Optionally sleeps, to stand in for a sink that blocks.
class RecordingSink : public IOutputSink {
 public:
  explicit RecordingSink(std::chrono::milliseconds delay =
                             std::chrono::milliseconds(0),
                         bool available = true)
      : delay_(delay), available_(available) {}

  void Send(const Command& command) override {
    if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
    std::lock_guard<std::mutex> lock(mu_);
    commands_.push_back(command);
  }

  void ReleaseAll() override {
    std::lock_guard<std::mutex> lock(mu_);
    ++release_all_calls_;
  }

  bool Available() const override { return available_; }

  std::vector<Command> Commands() const {
    std::lock_guard<std::mutex> lock(mu_);
    return commands_;
  }

  int ReleaseAllCalls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return release_all_calls_;
  }

  int CountKind(Command::Kind kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (const Command& c : commands_) {
      if (c.kind == kind) ++n;
    }
    return n;
  }

 private:
  mutable std::mutex mu_;
  std::vector<Command> commands_;
  int release_all_calls_ = 0;
  std::chrono::milliseconds delay_;
  bool available_;
};

// A NON-OWNING forwarder, and the reason every test below uses one.
//
// QueuedSink owns its inner sink through a unique_ptr and destroys it, so a
// test holding a raw pointer to that inner sink cannot inspect it afterwards
// -- and the destructor test's whole point is to inspect it afterwards. An
// earlier version of this file did exactly that and did not crash: it HUNG,
// locking the recorder's already-destroyed mutex. Handing QueuedSink a
// forwarder instead keeps the recorder owned by the test, which outlives it.
class SinkRef : public IOutputSink {
 public:
  explicit SinkRef(IOutputSink* target) : target_(target) {}
  void Send(const Command& command) override { target_->Send(command); }
  void ReleaseAll() override { target_->ReleaseAll(); }
  bool Available() const override { return target_->Available(); }

 private:
  IOutputSink* target_;
};

std::unique_ptr<IOutputSink> Ref(RecordingSink& sink) {
  return std::unique_ptr<IOutputSink>(new SinkRef(&sink));
}

Command Move(float dx) {
  Command c{};
  c.kind = Command::Kind::MoveCursorRelative;
  c.delta = Point2{dx, 0.0f};
  return c;
}

Command Release(Key k) {
  Command c{};
  c.kind = Command::Kind::KeyRelease;
  c.key = k;
  return c;
}

}  // namespace

// The inner sink sleeps 100 ms per command -- the pyautogui PAUSE this class
// exists to avoid. A hundred Sends must return in far less than ONE of those.
TEST(QueuedSink, SendReturnsImmediatelyWhenTheInnerSinkIsSlow) {
  RecordingSink recorder(std::chrono::milliseconds(100));
  QueuedSink sink(Ref(recorder), 8);

  const Clock::time_point start = Clock::now();
  for (int i = 0; i < 100; ++i) sink.Send(Move(0.01f));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - start);

  EXPECT_LT(elapsed.count(), 50)
      << "100 Sends took " << elapsed.count()
      << " ms; a single inner call is 100 ms, so Send is blocking";
}

TEST(QueuedSink, DropsOldestWhenFullRatherThanBlocking) {
  RecordingSink recorder(std::chrono::milliseconds(50));
  QueuedSink sink(Ref(recorder), 4);

  // Far more than capacity, with identifiable payloads.
  for (int i = 0; i < 40; ++i) sink.Send(Move(static_cast<float>(i)));

  EXPECT_GT(sink.DroppedCount(), 0u) << "nothing was dropped, so it blocked";
  EXPECT_TRUE(sink.Drain(std::chrono::milliseconds(3000)));

  const std::vector<Command> got = recorder.Commands();
  ASSERT_FALSE(got.empty());
  EXPECT_LT(got.size(), 40u) << "everything arrived, so nothing was dropped";

  // The newest survives: it describes where the hand is now. The oldest is
  // the one evicted.
  EXPECT_FLOAT_EQ(got.back().delta.x, 39.0f);
}

// A dropped cursor delta is a cursor that moved slightly less. A dropped
// release is a key held down after the hand is gone and after the process
// exits. The two are not comparable, so they are not treated the same.
TEST(QueuedSink, NeverDropsAReleaseCommand) {
  RecordingSink recorder(std::chrono::milliseconds(20));
  QueuedSink sink(Ref(recorder), 4);

  // Flood well past capacity so the queue is certainly full.
  for (int i = 0; i < 200; ++i) sink.Send(Move(0.01f));

  // Then the releases, into a full queue.
  sink.Send(Release(Key::ArrowUp));
  sink.Send(Release(Key::ArrowLeft));
  sink.Send(Release(Key::ArrowDown));

  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(recorder.CountKind(Command::Kind::KeyRelease), 3)
      << "a release was dropped; that is a stuck key";
}

// Review focus 7: every press must have a matching release on every exit
// path, and the destructor is the path that runs when the code above forgot.
TEST(QueuedSink, DestructorDrainsAndReleasesAll) {
  // Declared outside the scope below so it outlives the QueuedSink and can
  // still be inspected once the destructor has run.
  RecordingSink recorder;
  {
    QueuedSink sink(Ref(recorder), 64);
    for (int i = 0; i < 10; ++i) sink.Send(Move(0.01f));
    sink.Send(Release(Key::ArrowRight));
  }  // Destructor runs here.

  EXPECT_GE(recorder.ReleaseAllCalls(), 1)
      << "the destructor did not release the inner sink";
  EXPECT_EQ(recorder.CountKind(Command::Kind::KeyRelease), 1)
      << "the queued release was discarded instead of drained";
  EXPECT_EQ(recorder.CountKind(Command::Kind::MoveCursorRelative), 10)
      << "queued motion was discarded rather than drained";
}

TEST(QueuedSink, CountsDropsForReporting) {
  RecordingSink recorder(std::chrono::milliseconds(30));
  QueuedSink sink(Ref(recorder), 2);

  for (int i = 0; i < 50; ++i) sink.Send(Move(0.01f));
  const std::size_t dropped = sink.DroppedCount();

  EXPECT_GT(dropped, 0u);
  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));

  // Everything sent was either delivered or counted as dropped; nothing
  // vanished unaccounted for.
  const std::size_t delivered =
      static_cast<std::size_t>(recorder.CountKind(Command::Kind::MoveCursorRelative));
  EXPECT_EQ(delivered + dropped, 50u)
      << "delivered " << delivered << " + dropped " << dropped
      << " should account for all 50";
}

// The deterministic version of the defect the concurrent test below found by
// luck: a release already SITTING in the queue must survive the eviction
// caused by later motion. An unconditional pop_front() -- the obvious way to
// implement "drop the oldest" -- throws the release away, and the key stays
// down. Written separately so a regression fails here, by name, rather than
// turning up as one missing release in a thread-timing test.
TEST(QueuedSink, AQueuedReleaseSurvivesLaterEvictions) {
  RecordingSink recorder(std::chrono::milliseconds(40));
  QueuedSink sink(Ref(recorder), 4);

  // Fill the queue, so the worker is busy and nothing drains meanwhile.
  for (int i = 0; i < 4; ++i) sink.Send(Move(0.01f));

  // The release goes in while the queue is already full.
  sink.Send(Release(Key::ArrowUp));

  // Now bury it under far more motion than the queue can hold. Every one of
  // these has to evict something, and none of them may evict the release.
  for (int i = 0; i < 100; ++i) sink.Send(Move(0.02f));

  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(recorder.CountKind(Command::Kind::KeyRelease), 1)
      << "the queued release was evicted by later motion; that is a stuck key";
}

// --- the rest of the contract --------------------------------------------

TEST(QueuedSink, ReleaseAllBlocksUntilTheInnerSinkHasActuallyReleased) {
  RecordingSink recorder(std::chrono::milliseconds(30));
  QueuedSink sink(Ref(recorder), 64);

  for (int i = 0; i < 5; ++i) sink.Send(Move(0.01f));
  EXPECT_EQ(recorder.ReleaseAllCalls(), 0);

  sink.ReleaseAll();

  // Unlike Send, this one has returned only once the work is done. A
  // non-blocking "please release eventually" would not be a guarantee.
  EXPECT_GE(recorder.ReleaseAllCalls(), 1);
}

TEST(QueuedSink, AvailabilityComesFromTheInnerSink) {
  RecordingSink good(std::chrono::milliseconds(0), true);
  QueuedSink ok(Ref(good), 8);
  EXPECT_TRUE(ok.Available());

  RecordingSink unavailable(std::chrono::milliseconds(0), false);
  QueuedSink broken(Ref(unavailable), 8);
  EXPECT_FALSE(broken.Available())
      << "an unavailable inner sink must not look available behind the queue";

  // And an unavailable sink is still safe to send to.
  broken.Send(Move(0.1f));
  broken.ReleaseAll();
}

TEST(QueuedSink, DeliversInOrder) {
  RecordingSink recorder;
  QueuedSink sink(Ref(recorder), 256);

  for (int i = 0; i < 100; ++i) sink.Send(Move(static_cast<float>(i)));
  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));

  const std::vector<Command> got = recorder.Commands();
  ASSERT_EQ(got.size(), 100u) << "capacity was ample; nothing should drop";
  for (int i = 0; i < 100; ++i) {
    EXPECT_FLOAT_EQ(got[static_cast<std::size_t>(i)].delta.x,
                    static_cast<float>(i))
        << "reordered at " << i;
  }
}

TEST(QueuedSink, DrainOnAnIdleSinkReturnsImmediately) {
  RecordingSink recorder;
  QueuedSink sink(Ref(recorder), 8);
  const Clock::time_point start = Clock::now();
  EXPECT_TRUE(sink.Drain(std::chrono::milliseconds(2000)));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start)
                .count(),
            100);
}

TEST(QueuedSink, SurvivesConcurrentSendersAndLosesNoRelease) {
  RecordingSink recorder;
  QueuedSink sink(Ref(recorder), 16);

  // Four threads hammering Send at once. The frame loop is single-threaded
  // today, but the Controller of Task 11 is meant to be usable from a server
  // handling many sessions, and a queue that corrupts under concurrent use
  // would be discovered there rather than here.
  std::vector<std::thread> senders;
  std::atomic<int> releases_sent{0};
  for (int t = 0; t < 4; ++t) {
    senders.emplace_back([&sink, &releases_sent] {
      for (int i = 0; i < 250; ++i) sink.Send(Move(0.001f));
      sink.Send(Release(Key::Space));
      ++releases_sent;
    });
  }
  for (std::thread& t : senders) t.join();

  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(releases_sent.load(), 4);
  EXPECT_EQ(recorder.CountKind(Command::Kind::KeyRelease), 4)
      << "a release was lost under concurrent senders";
}

// Capacity 1 is the degenerate configuration and must still honour the
// undroppable rule rather than falling into a special case.
TEST(QueuedSink, CapacityOneStillNeverDropsARelease) {
  RecordingSink recorder(std::chrono::milliseconds(10));
  QueuedSink sink(Ref(recorder), 1);

  for (int i = 0; i < 50; ++i) sink.Send(Move(0.01f));
  sink.Send(Release(Key::Enter));

  ASSERT_TRUE(sink.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(recorder.CountKind(Command::Kind::KeyRelease), 1);
}
