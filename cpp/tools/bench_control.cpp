// Measures the per-frame cost of layers 3-5 -- gesture recognition, action
// mapping and command dispatch -- in isolation from the tracker.
//
// WHY A SEPARATE TOOL rather than a Metrics stage in the application: there is
// no camera on this machine, so a run of the real binary sees no hands, and
// layers 3-5 over an empty observation list cost nothing and would measure
// nothing. This feeds them two hands holding bound gestures on every frame,
// which is the worst case the owner's target behaviour produces.
//
//   cmake --build cpp/build --target bench_control
//   ./cpp/build/tools/bench_control
//
// Reports nanoseconds per frame for each layer, and what that is as a share
// of the ~11 ms the phase 1 report measured for the tracker. The expectation
// is that it is negligible -- but the project's method is to measure rather
// than to assume, and if it is not negligible that is worth knowing.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "hc/action_mapper.hpp"
#include "hc/bindings.hpp"
#include "hc/command.hpp"
#include "hc/gesture_event.hpp"
#include "hc/gesture_recognizer.hpp"
#include "hc/hand_observation.hpp"
#include "hc/null_sink.hpp"
#include "hc/output_sink.hpp"
#include "hc/queued_sink.hpp"

namespace {

// The per-frame cost the phase 1 report measured for the tracker, for the
// share column. Not re-measured here: this tool deliberately does not load a
// model.
constexpr double kTrackerMsPerFrame = 11.0;

constexpr int kFrames = 200000;

// --- synthetic poses ------------------------------------------------------
//
// The same upright-hand layout the recognizer tests use. Duplicated here
// because tools/ cannot include tests/; consolidating the four copies in the
// repository is tracked separately.

enum Lm {
  kWrist = 0, kThumbIp = 3, kThumbTip = 4, kIndexMcp = 5,
};

constexpr float kCol[4] = {-0.05f, -0.01f, 0.03f, 0.07f};
constexpr int kMcp[4] = {5, 9, 13, 17};
constexpr int kPip[4] = {6, 10, 14, 18};
constexpr int kDip[4] = {7, 11, 15, 19};
constexpr int kTip[4] = {8, 12, 16, 20};

hc::HandObservation OpenPalm(std::uint32_t track_id, hc::Handedness hand,
                             hc::Point2 origin) {
  hc::HandObservation o{};
  o.track_id = track_id;
  o.handedness = hand;
  o.handedness_score = 0.9f;
  o.presence = 1.0f;

  const auto put = [&](int i, float dx, float dy) {
    o.landmarks[i] = hc::Point2{origin.x + dx, origin.y + dy};
  };
  put(kWrist, 0.0f, 0.0f);
  put(1, -0.03f, -0.02f);
  put(2, -0.07f, -0.05f);
  put(kThumbIp, -0.11f, -0.07f);
  put(kThumbTip, -0.20f, -0.09f);
  for (int f = 0; f < 4; ++f) {
    put(kMcp[f], kCol[f], -0.10f);
    put(kPip[f], kCol[f], -0.18f);
    put(kDip[f], kCol[f], -0.24f);
    put(kTip[f], kCol[f], -0.30f);
  }
  return o;
}

void Curl(hc::HandObservation& o, int f) {
  const hc::Point2 pip = o.landmarks[kPip[f]];
  o.landmarks[kDip[f]] = hc::Point2{pip.x, pip.y + 0.03f};
  o.landmarks[kTip[f]] = hc::Point2{pip.x, pip.y + 0.06f};
}

void FoldThumb(hc::HandObservation& o) {
  const hc::Point2 index_mcp = o.landmarks[kIndexMcp];
  o.landmarks[kThumbIp] = hc::Point2{index_mcp.x - 0.01f, index_mcp.y + 0.02f};
  o.landmarks[kThumbTip] = hc::Point2{index_mcp.x + 0.01f, index_mcp.y + 0.01f};
}

// Right hand pointing: bound to move_cursor on `held`, so it produces a
// command on every frame.
hc::HandObservation PointIndex(std::uint32_t id, hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(id, hc::Handedness::Right, origin);
  for (int f = 1; f < 4; ++f) Curl(o, f);
  FoldThumb(o);
  return o;
}

// Left thumb-up: bound to a held arrow key, so one key stays down for the
// whole run -- the owner's target behaviour, and the state the recognizer
// does the most work in.
hc::HandObservation ThumbUp(std::uint32_t id, hc::Point2 origin) {
  hc::HandObservation o = OpenPalm(id, hc::Handedness::Left, origin);
  for (int f = 0; f < 4; ++f) Curl(o, f);
  return o;
}

std::vector<hc::HandObservation> FrameHands(int frame) {
  // Drifting, so the mapper's smoothing has real motion to filter rather than
  // a stream of zeroes it could in principle shortcut.
  const float drift = static_cast<float>(frame % 100) * 0.001f;
  return {PointIndex(1, {0.45f + drift, 0.80f}),
          ThumbUp(2, {0.20f, 0.80f})};
}

double MsPerFrame(std::chrono::nanoseconds total) {
  return static_cast<double>(total.count()) / kFrames / 1e6;
}

void Report(const char* label, std::chrono::nanoseconds total) {
  const double ms = MsPerFrame(total);
  std::printf("%-26s %9.1f ns/frame   %8.4f ms/frame   %6.3f%% of tracker\n",
              label, ms * 1e6, ms, 100.0 * ms / kTrackerMsPerFrame);
}

}  // namespace

int main() {
  hc::BindingSet bindings;
  std::string error;
  if (!hc::ParseBindings(hc::DefaultBindingsText(), bindings, error)) {
    std::fprintf(stderr, "the shipped default does not parse: %s\n",
                 error.c_str());
    return 1;
  }

  // Built once, outside the timed loops: a tool that measured construction
  // would be measuring the one thing that happens once per session.
  std::vector<std::vector<hc::HandObservation>> frames;
  frames.reserve(100);
  for (int i = 0; i < 100; ++i) frames.push_back(FrameHands(i));

  std::printf("bench_control: %d frames, two hands, shipped bindings\n",
              kFrames);
  std::printf("tracker baseline %.1f ms/frame (phase 1 report, two hands)\n\n",
              kTrackerMsPerFrame);

  // --- layer 3: gesture recognition --------------------------------------
  {
    hc::GestureRecognizer recognizer(hc::GestureConfig::Default());
    std::vector<hc::GestureEvent> events;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      recognizer.Process(frames[i % 100], static_cast<std::int64_t>(i) * 11000,
                         events);
    }
    const auto end = std::chrono::steady_clock::now();
    Report("recognizer", end - start);
  }

  // --- layers 3 + 4: recognition and mapping -----------------------------
  {
    hc::GestureRecognizer recognizer(hc::GestureConfig::Default());
    hc::ActionMapper mapper(bindings);
    std::vector<hc::GestureEvent> events;
    std::vector<hc::Command> commands;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      recognizer.Process(frames[i % 100], static_cast<std::int64_t>(i) * 11000,
                         events);
      mapper.Map(events, commands);
    }
    const auto end = std::chrono::steady_clock::now();
    Report("recognizer + mapper", end - start);
  }

  // --- layers 3 + 4 + 5: through the null sink ---------------------------
  //
  // The default configuration's dispatch path. A virtual call per command
  // into a sink that discards: the floor of what dispatch can cost.
  {
    hc::GestureRecognizer recognizer(hc::GestureConfig::Default());
    hc::ActionMapper mapper(bindings);
    hc::NullSink sink;
    std::vector<hc::GestureEvent> events;
    std::vector<hc::Command> commands;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      recognizer.Process(frames[i % 100], static_cast<std::int64_t>(i) * 11000,
                         events);
      mapper.Map(events, commands);
      for (const hc::Command& c : commands) sink.Send(c);
    }
    const auto end = std::chrono::steady_clock::now();
    Report("+ null sink", end - start);
  }

  // --- layers 3 + 4 + 5: through the dispatch queue ----------------------
  //
  // What `--sink xtest` and `--sink log` actually pay on the frame loop: a
  // lock, a push and a condition-variable notify. The inner sink's own cost
  // is paid on the worker thread and is deliberately NOT in this number --
  // that is the whole point of the queue.
  {
    hc::GestureRecognizer recognizer(hc::GestureConfig::Default());
    hc::ActionMapper mapper(bindings);
    hc::QueuedSink sink(std::make_unique<hc::NullSink>(), 256);
    std::vector<hc::GestureEvent> events;
    std::vector<hc::Command> commands;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
      recognizer.Process(frames[i % 100], static_cast<std::int64_t>(i) * 11000,
                         events);
      mapper.Map(events, commands);
      for (const hc::Command& c : commands) sink.Send(c);
    }
    const auto end = std::chrono::steady_clock::now();
    Report("+ queued sink", end - start);

    std::printf("\nqueue dropped %zu commands (worker could not keep up with\n"
                "a frame loop running flat out; a real one runs at ~90 fps)\n",
                sink.DroppedCount());
  }

  return 0;
}
