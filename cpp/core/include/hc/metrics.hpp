#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hc {

// Stages in the hand-tracking pipeline, used for per-stage timing metrics.
enum class Stage {
  CaptureWait,  // Waiting for a frame from the camera.
  Preprocess,   // Image preprocessing (resizing, format conversion, etc.).
  PalmDetect,   // Palm detection inference.
  Landmark,     // Landmark inference and hand pose extraction.
  Associate,    // Track association and de-duplication.
  Total,        // Total end-to-end time per frame.
  kCount        // Number of stages.
};

// Statistics for a single stage's samples.
struct StageStats {
  // Total number of Add() + Bump() invocations for this stage. Bump() counts
  // without recording a duration, so `calls` can exceed the number of
  // samples the three duration fields below are computed from.
  std::uint64_t calls;
  // Computed only over Add()-recorded samples; all three are 0.0 when the
  // stage has no samples, even if `calls` is non-zero.
  double mean_ms;        // Mean duration across all samples.
  double p50_ms;         // 50th percentile (median).
  double p95_ms;         // 95th percentile.
};

// Accumulator for per-stage timing metrics. Stores samples per stage and
// computes percentiles on demand. Thread-unsafe.
class Metrics {
 public:
  // Records a single sample for stage `s` with duration `ms` milliseconds.
  void Add(Stage s, double ms);

  // Increments the call count for stage `s` without recording a duration.
  // Used for call-count-only metrics like the palm detector invocation count.
  void Bump(Stage s);

  // Returns statistics for stage `s`. An untouched stage yields calls == 0
  // and all durations 0.0.
  StageStats Get(Stage s) const;

  // Returns a formatted report, one line per stage that ran. A stage with
  // calls == 0 is omitted entirely. A stage with calls but no recorded
  // durations (counted via Bump only) appears with its call count and `n/a`
  // in place of each duration, so the report never claims a stage never ran
  // and never shows a misleading 0.00 ms row.
  std::string Format() const;

  // Clears all samples and counts for all stages.
  void Reset();

 private:
  // Samples for each stage, in milliseconds.
  std::vector<double> samples_[static_cast<int>(Stage::kCount)];
  // Call counts for each stage (incremented by both Add and Bump).
  std::uint64_t calls_[static_cast<int>(Stage::kCount)] = {};
};

// RAII helper that times its own lifetime and records the duration to a stage.
class ScopedStage {
 public:
  // Start timing. On destruction, records a sample to `stage`.
  ScopedStage(Metrics& m, Stage stage);
  ~ScopedStage();

 private:
  Metrics& metrics_;
  Stage stage_;
  // Construction timestamp from std::chrono::high_resolution_clock; the dtor
  // records the elapsed time since it via Add().
  double start_time_ms_;
};

}  // namespace hc
