#pragma once

#include "hc/output_sink.hpp"

namespace hc {

// A sink that discards everything, and the DEFAULT one the application runs
// with until told otherwise.
//
// That default is not timidity. Development happens on the same desktop the
// program would seize, and a build that grabs the cursor the moment it starts
// is hostile to the person writing it.
//
// Available() is TRUE, which is the distinction that matters here: this sink
// works perfectly and discarding is its job. An unavailable sink is a broken
// one -- an X11 sink on a machine with no X11 -- and the application reports
// those differently.
//
// Lives in hc_control rather than platform/ because there is nothing
// platform-specific about it, and because the headless, server and browser
// configurations all need it: those are precisely the builds that must not
// require OpenCV.
class NullSink : public IOutputSink {
 public:
  void Send(const Command&) override {}
  void ReleaseAll() override {}
  bool Available() const override { return true; }
};

}  // namespace hc
