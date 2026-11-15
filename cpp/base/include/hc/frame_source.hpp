#pragma once

#include <cstdint>

#include "hc/image_view.hpp"

namespace hc {

// One captured frame: a non-owning view of its pixels plus the metadata the
// pipeline needs to reason about time and about dropped frames.
struct Frame {
  // Non-owning (see ImageView). The storage belongs to the IFrameSource that
  // produced this frame, and the lifetime rule is the one documented on
  // IFrameSource::Next below.
  ImageView view;

  // Capture time in microseconds on a steady, monotonic clock. Only
  // differences between timestamps are meaningful; the epoch is unspecified.
  std::int64_t timestamp_us;

  // The PRODUCING source's own monotonic counter: it starts at 0 and
  // increments by exactly one for every frame that source produces. It is
  // NOT a count of the frames the consumer received.
  //
  // This distinction is the point of the field. LatestFrameSource passes the
  // inner source's index through UNCHANGED, so when it drops stale frames
  // the consumer sees a gap larger than one between successive Next()
  // results -- that gap is how dropped frames become observable at all.
  // (LatestFrameSource::dropped() counts the same event directly.)
  std::uint64_t index;
};

// A source of frames: a camera, a video file, a synthetic generator, or a
// decorator over another source.
//
// Unless a concrete source documents otherwise, an IFrameSource is NOT
// thread-safe: Next() and Close() must not be called concurrently from
// different threads on the same object. (LatestFrameSource is the exception
// and documents its own rules.)
class IFrameSource {
 public:
  virtual ~IFrameSource() = default;

  // Fetches the next frame. Returns true and fills `out` on success.
  //
  // Returns false when the source has ended or failed, and once it has
  // returned false it returns false forever -- consumers may treat the first
  // false as terminal and stop polling.
  //
  // LIFETIME: `out.view` is non-owning and points into storage owned by this
  // source. It stays valid only until the next Next() or Close() call on
  // this source, or until the source is destroyed, whichever comes first. A
  // consumer that needs the pixels beyond that point must copy them. Every
  // implementation of IFrameSource must honour this contract; none of them
  // promises more.
  virtual bool Next(Frame& out) = 0;

  // Releases the source's resources and ends it: after Close(), Next()
  // returns false forever. Close() is idempotent -- calling it twice, or
  // calling it after the source has already ended on its own, is safe and
  // has no further effect. The destructor closes the source if the caller
  // has not.
  virtual void Close() = 0;
};

}  // namespace hc
