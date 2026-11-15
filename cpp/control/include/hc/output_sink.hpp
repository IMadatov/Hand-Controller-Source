#pragma once

#include "hc/command.hpp"

namespace hc {

// Where commands go. The interface is core because it names no platform type;
// every implementation that does is in platform/.
//
// THIS IS THE BOUNDARY WHERE NORMALIZED COORDINATES END. Phase 1 established
// that the core speaks 0..1 everywhere and that DrawObservations was the only
// place multiplying by pixel dimensions. A sink is the second and last such
// place, because screen coordinates are pixels. Nothing above this interface
// may learn a resolution.
class IOutputSink {
 public:
  virtual ~IOutputSink() = default;

  // Performs one command, or does nothing if this sink cannot.
  //
  // MUST NOT BLOCK THE CALLER for any meaningful time. A sink that can block
  // -- which on X11 means any of them, since the server can stall -- belongs
  // behind QueuedSink rather than in a frame loop.
  virtual void Send(const Command& command) = 0;

  // Releases every button and key this sink currently holds down, and holds
  // nothing afterwards.
  //
  // Review focus 7, and the reason this is on the interface rather than being
  // left to each sink's destructor: a key still down outlives the process. The
  // application must call this on EVERY exit path, including the abnormal
  // ones, and it must be safe to call twice or on a sink holding nothing.
  virtual void ReleaseAll() = 0;

  // False when this sink cannot actually deliver commands.
  //
  // This exists because of the exact trap the Wayland probe exposed: XTest
  // reports its extension present at version 2.2, returns success from every
  // call, and moves nothing. A sink that cannot work must SAY SO here so the
  // application can tell the user, rather than appearing to run while doing
  // nothing at all.
  //
  // An unavailable sink must still be safe to Send to -- it simply discards.
  virtual bool Available() const = 0;
};

}  // namespace hc
