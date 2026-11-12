#include "hc/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace hc {

namespace {

// Helper to get the stage name for formatting.
const char* StageName(Stage s) {
  switch (s) {
    case Stage::CaptureWait:
      return "capturewait";
    case Stage::Preprocess:
      return "preprocess";
    case Stage::PalmDetect:
      return "palmdetect";
    case Stage::Landmark:
      return "landmark";
    case Stage::Associate:
      return "associate";
    case Stage::Total:
      return "total";
    case Stage::kCount:
      return "kcount";  // Should never appear in Format(), but be explicit.
  }
  return "unknown";
}

// Computes the p-th percentile of a vector of values. Assumes the vector
// is sorted. If the percentile index is fractional, linearly interpolates.
double Percentile(const std::vector<double>& sorted_samples, double p) {
  if (sorted_samples.empty()) return 0.0;
  if (sorted_samples.size() == 1) return sorted_samples[0];

  // p is in [0, 100]. Convert to a 0-indexed position in [0, size-1].
  double idx = (p / 100.0) * (sorted_samples.size() - 1);
  int lower = static_cast<int>(std::floor(idx));
  int upper = static_cast<int>(std::ceil(idx));

  if (lower == upper) return sorted_samples[lower];

  double frac = idx - lower;
  return sorted_samples[lower] * (1.0 - frac) +
         sorted_samples[upper] * frac;
}

}  // namespace

void Metrics::Add(Stage s, double ms) {
  const int idx = static_cast<int>(s);
  samples_[idx].push_back(ms);
  calls_[idx]++;
}

void Metrics::Bump(Stage s) {
  const int idx = static_cast<int>(s);
  calls_[idx]++;
}

StageStats Metrics::Get(Stage s) const {
  const int idx = static_cast<int>(s);
  const auto& samples = samples_[idx];
  const auto call_count = calls_[idx];

  if (samples.empty()) {
    return StageStats{call_count, 0.0, 0.0, 0.0};
  }

  // Compute mean.
  double sum = 0.0;
  for (double sample : samples) {
    sum += sample;
  }
  const double mean = sum / samples.size();

  // Sort a copy to compute percentiles.
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());

  const double p50 = Percentile(sorted, 50.0);
  const double p95 = Percentile(sorted, 95.0);

  return StageStats{call_count, mean, p50, p95};
}

std::string Metrics::Format() const {
  std::ostringstream oss;
  bool first = true;

  for (int i = 0; i < static_cast<int>(Stage::kCount); ++i) {
    const auto& samples = samples_[i];
    if (samples.empty()) continue;  // Skip stages with no samples.

    if (!first) oss << "\n";
    first = false;

    const StageStats stats = Get(static_cast<Stage>(i));
    oss << StageName(static_cast<Stage>(i))
        << ": calls=" << stats.calls
        << " mean=" << stats.mean_ms
        << "ms p50=" << stats.p50_ms
        << "ms p95=" << stats.p95_ms << "ms";
  }

  return oss.str();
}

void Metrics::Reset() {
  for (int i = 0; i < static_cast<int>(Stage::kCount); ++i) {
    samples_[i].clear();
    calls_[i] = 0;
  }
}

// ScopedStage implementation.
ScopedStage::ScopedStage(Metrics& m, Stage stage)
    : metrics_(m), stage_(stage) {
  auto now =
      std::chrono::high_resolution_clock::now().time_since_epoch();
  auto millis =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          now);
  start_time_ms_ = millis.count();
}

ScopedStage::~ScopedStage() {
  auto now =
      std::chrono::high_resolution_clock::now().time_since_epoch();
  auto millis =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          now);
  double end_time_ms = millis.count();
  double elapsed_ms = end_time_ms - start_time_ms_;

  metrics_.Add(stage_, elapsed_ms);
}

}  // namespace hc
