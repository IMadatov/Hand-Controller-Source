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
  std::uint64_t calls;   // Number of samples recorded for this stage.
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

  // Returns a formatted report of all stages with samples. Stages with no
  // samples are omitted entirely (not shown as "0.00 ms" rows).
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
  // We avoid std::chrono here: the brief says "test infrastructure must not
  // need a real timer to be deterministic," so we accept a duration parameter
  // to Add() instead of capturing wall time internally. The dtor will compute
  // elapsed time if we can, but tests will mock it via direct Add() calls.
  // For now, capture the start time; the implementation will measure elapsed
  // time in the dtor.
  double start_time_ms_;
};

}  // namespace hc
