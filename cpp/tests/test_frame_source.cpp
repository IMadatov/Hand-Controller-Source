#include "hc/frame_source.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hc/image_view.hpp"
#include "hc/latest_frame_source.hpp"
#include "hc/synthetic_frame_source.hpp"

using hc::Frame;
using hc::IFrameSource;
using hc::LatestFrameSource;
using hc::PixelFormat;
using hc::SyntheticFrameSource;

namespace {

// Copies the pixel bytes a frame points at into an owned vector, so a test
// can compare two frames (or the same frame before and after the producer
// has had time to run) without depending on the source's storage staying
// put.
std::vector<std::uint8_t> CopyPixels(const Frame& f) {
  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(f.view.height) * f.view.width * 3);
  for (int y = 0; y < f.view.height; ++y) {
    const std::uint8_t* row = f.view.data + static_cast<std::size_t>(y) * f.view.stride;
    out.insert(out.end(), row, row + static_cast<std::size_t>(f.view.width) * 3);
  }
  return out;
}

// An inner source whose Next() fails immediately and forever: the
// "phone camera never connected" case.
class FailingFrameSource : public IFrameSource {
 public:
  bool Next(Frame&) override { return false; }
  void Close() override {}
};

// An inner source whose Next() blocks for a long time and then fails. Used
// to pin that Close() wakes a consumer already blocked in Next() rather
// than leaving it parked until the source happens to end on its own.
class StallingFrameSource : public IFrameSource {
 public:
  explicit StallingFrameSource(std::chrono::milliseconds stall) : stall_(stall) {}

  bool Next(Frame&) override {
    std::this_thread::sleep_for(stall_);
    return false;
  }
  void Close() override {}

 private:
  std::chrono::milliseconds stall_;
};

}  // namespace

TEST(FrameSource, SyntheticYieldsRequestedCountThenEnds) {
  SyntheticFrameSource src(16, 12, 3);
  Frame f{};

  ASSERT_TRUE(src.Next(f));
  EXPECT_EQ(f.index, 0u);
  EXPECT_EQ(f.view.width, 16);
  EXPECT_EQ(f.view.height, 12);
  EXPECT_NE(f.view.data, nullptr);
  EXPECT_GE(f.view.stride, 16 * 3);

  ASSERT_TRUE(src.Next(f));
  EXPECT_EQ(f.index, 1u);

  ASSERT_TRUE(src.Next(f));
  EXPECT_EQ(f.index, 2u);

  // False once and forever.
  EXPECT_FALSE(src.Next(f));
  EXPECT_FALSE(src.Next(f));
}

TEST(FrameSource, SyntheticFramesDiffer) {
  SyntheticFrameSource src(16, 12, -1);

  Frame f0{};
  ASSERT_TRUE(src.Next(f0));
  const std::vector<std::uint8_t> p0 = CopyPixels(f0);

  Frame f1{};
  ASSERT_TRUE(src.Next(f1));
  const std::vector<std::uint8_t> p1 = CopyPixels(f1);

  ASSERT_EQ(p0.size(), p1.size());
  EXPECT_NE(p0, p1);
}

TEST(FrameSource, SyntheticUnlimitedKeepsProducing) {
  SyntheticFrameSource src(8, 8, -1);
  Frame f{};
  for (std::uint64_t i = 0; i < 50; ++i) {
    ASSERT_TRUE(src.Next(f));
    EXPECT_EQ(f.index, i);
  }
}

TEST(FrameSource, SyntheticCloseEndsSource) {
  SyntheticFrameSource src(8, 8, -1);
  Frame f{};
  ASSERT_TRUE(src.Next(f));
  src.Close();
  EXPECT_FALSE(src.Next(f));
  src.Close();  // Idempotent.
  EXPECT_FALSE(src.Next(f));
}

// A latest-frame source by construction does NOT forward every frame -- that
// is what dropped() exists for. What it does guarantee is forward progress
// with strictly increasing, never-repeated producer indices: a source that
// handed the same frame back ten times would be broken, and must fail here.
TEST(FrameSource, LatestYieldsStrictlyIncreasingNonDuplicatedIndices) {
  LatestFrameSource src(std::make_unique<SyntheticFrameSource>(16, 12, -1));

  std::set<std::uint64_t> seen;
  std::uint64_t previous = 0;
  int received = 0;
  Frame f{};
  for (int i = 0; i < 10; ++i) {
    if (!src.Next(f)) break;
    if (received > 0) {
      EXPECT_GT(f.index, previous) << "indices must strictly increase at i=" << i;
    }
    EXPECT_TRUE(seen.insert(f.index).second) << "duplicate index " << f.index;
    previous = f.index;
    ++received;
  }

  EXPECT_GT(received, 0);
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(received));
}

// Timing-dependent by nature: assert only that SOME dropping happened and
// that the producer index gapped, never an exact count or an exact index.
TEST(FrameSource, LatestDropsStaleFramesWhenConsumerIsSlow) {
  LatestFrameSource src(std::make_unique<SyntheticFrameSource>(64, 48, -1));

  Frame first{};
  ASSERT_TRUE(src.Next(first));

  // An order of magnitude longer than the producer's per-frame cost, so the
  // producer has certainly made many frames we will never see.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  Frame second{};
  ASSERT_TRUE(src.Next(second));

  EXPECT_GT(src.dropped(), 0u);
  EXPECT_GT(second.index, first.index + 1)
      << "producer index must gap when frames are dropped";
}

// Three-buffer rotation: the view handed to the consumer must stay stable
// until the next Next() call, even while the capture thread keeps running
// at full speed. A two-buffer implementation would be writing into the
// buffer the consumer is holding.
TEST(FrameSource, LatestHeldViewIsNotOverwrittenByProducer) {
  LatestFrameSource src(std::make_unique<SyntheticFrameSource>(64, 48, -1));

  Frame held{};
  ASSERT_TRUE(src.Next(held));
  const std::vector<std::uint8_t> snapshot = CopyPixels(held);

  // Let the producer churn through many frames while we keep holding.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(CopyPixels(held), snapshot)
      << "the held frame's pixels changed under the consumer";
}

TEST(FrameSource, LatestEndsWhenInnerSourceEnds) {
  LatestFrameSource src(std::make_unique<SyntheticFrameSource>(16, 12, 3));

  Frame f{};
  const auto start = std::chrono::steady_clock::now();
  int received = 0;
  while (src.Next(f)) {
    ++received;
    ASSERT_LT(received, 100) << "a 3-frame source produced too many frames";
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_GT(received, 0);
  EXPECT_LE(received, 3);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);

  // False once and forever.
  EXPECT_FALSE(src.Next(f));
  EXPECT_FALSE(src.Next(f));
}

TEST(FrameSource, LatestSurvivesInnerFailureOnFirstFrame) {
  LatestFrameSource src(std::make_unique<FailingFrameSource>());

  Frame f{};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(src.Next(f));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000)
      << "Next() must fail promptly, not hang";
  EXPECT_FALSE(src.Next(f));
}

TEST(FrameSource, LatestDestructorJoinsCleanly) {
  for (int i = 0; i < 5; ++i) {
    LatestFrameSource src(std::make_unique<SyntheticFrameSource>(32, 24, -1));
    Frame f{};
    if (i % 2 == 0) {
      // Destroy with the capture thread mid-flight and a frame held.
      (void)src.Next(f);
    }
    // No Close() call: the destructor must join on its own.
  }
  SUCCEED();
}

TEST(FrameSource, CloseIsIdempotent) {
  LatestFrameSource src(std::make_unique<SyntheticFrameSource>(32, 24, -1));

  Frame f{};
  (void)src.Next(f);
  src.Close();
  src.Close();  // Must not double-join and must not throw.

  EXPECT_FALSE(src.Next(f));
  EXPECT_FALSE(src.Next(f));
}

// Close() must wake a consumer already blocked inside Next(), rather than
// leaving it parked until the inner source happens to end by itself.
TEST(FrameSource, CloseWakesBlockedNext) {
  constexpr auto kStall = std::chrono::milliseconds(1500);
  LatestFrameSource src(std::make_unique<StallingFrameSource>(kStall));

  std::atomic<bool> result{true};
  std::atomic<long long> waited_ms{-1};
  std::atomic<bool> consumer_started{false};

  std::thread consumer([&] {
    Frame f{};
    consumer_started = true;
    const auto start = std::chrono::steady_clock::now();
    result = src.Next(f);
    waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
  });

  while (!consumer_started) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Close() itself has to wait for the stalled inner Next() to return before
  // it can join, so run it off the main thread and check the consumer first.
  std::thread closer([&] { src.Close(); });

  consumer.join();
  EXPECT_FALSE(result.load());
  EXPECT_LT(waited_ms.load(), 1000)
      << "blocked Next() was not woken by Close()";

  closer.join();
}
