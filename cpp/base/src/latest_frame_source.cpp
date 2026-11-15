#include "hc/latest_frame_source.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <utility>

namespace hc {

LatestFrameSource::LatestFrameSource(std::unique_ptr<IFrameSource> inner)
    : inner_(std::move(inner)) {
  assert(inner_ != nullptr);
  thread_ = std::thread(&LatestFrameSource::CaptureLoop, this);
}

LatestFrameSource::~LatestFrameSource() { Close(); }

void LatestFrameSource::StoreInto(int slot, const Frame& in) {
  Slot& s = slots_[static_cast<std::size_t>(slot)];
  const int width = in.view.width;
  const int height = in.view.height;
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;
  s.pixels.resize(row_bytes * static_cast<std::size_t>(height));

  // Copy row by row: the inner source's stride may exceed width * 3, and the
  // padding is not ours to carry around. Our own copy is always packed.
  for (int y = 0; y < height; ++y) {
    std::memcpy(s.pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                in.view.data + static_cast<std::size_t>(y) * in.view.stride, row_bytes);
  }

  s.frame = in;
  s.frame.view.data = s.pixels.data();
  s.frame.view.stride = static_cast<int>(row_bytes);
}

void LatestFrameSource::CaptureLoop() {
  for (;;) {
    int slot;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return;
      slot = capturing_;
    }

    Frame in{};
    if (!inner_->Next(in)) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        inner_ended_ = true;
      }
      cv_.notify_all();
      return;
    }

    // Outside the lock: `slot` is owned exclusively by this thread, and the
    // rotation below guarantees it is never the slot the consumer holds.
    StoreInto(slot, in);

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return;
      if (ready_ != -1) {
        // The consumer never took the previously published frame; that frame
        // is dropped and its slot becomes the one we capture into next.
        ++dropped_;
        capturing_ = ready_;
      } else {
        // Take the one slot that is neither the one we just filled nor the
        // one the consumer is holding.
        for (int i = 0; i < 3; ++i) {
          if (i != slot && i != held_) {
            capturing_ = i;
            break;
          }
        }
      }
      ready_ = slot;
      assert(capturing_ != ready_ && capturing_ != held_);
    }
    cv_.notify_one();
  }
}

bool LatestFrameSource::Next(Frame& out) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [this] { return closed_ || done_ || ready_ != -1 || inner_ended_; });

  if (closed_ || done_ || ready_ == -1) {
    // Closed, already terminal, or the inner source ended with nothing left.
    done_ = true;
    return false;
  }

  // Rotating here is what releases the consumer's previous slot: whatever
  // held_ was now becomes free for the capture thread to claim.
  held_ = ready_;
  ready_ = -1;
  out = slots_[static_cast<std::size_t>(held_)].frame;
  return true;
}

void LatestFrameSource::Close() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!closed_) {
      closed_ = true;
      done_ = true;
    }
  }
  // Notify unconditionally and outside the lock: a second Close() racing a
  // consumer that is still blocked must not leave it parked.
  cv_.notify_all();

  // `joined_` under its own mutex makes this idempotent even when two
  // threads call Close() at once: the second blocks here and then finds the
  // work already done, rather than double-joining.
  std::lock_guard<std::mutex> jlk(close_mu_);
  if (joined_) return;
  if (thread_.joinable()) thread_.join();
  joined_ = true;

  // Safe only here: the capture thread, the inner source's only other user,
  // has been joined.
  if (inner_) inner_->Close();
}

std::uint64_t LatestFrameSource::dropped() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dropped_;
}

}  // namespace hc
